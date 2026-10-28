package javascriptlimits

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
	"google.golang.org/api/option"

	"github.com/johejo/bigquery-emulator-duckdb/internal/serverprocess"
)

func binary(t *testing.T) string {
	t.Helper()
	path := os.Getenv("BQ_EMULATOR_BINARY")
	if path == "" {
		t.Skip("BQ_EMULATOR_BINARY is not set; run just e2e")
	}
	return path
}

// A client of an emulator of its own, started with args.
func newClient(t *testing.T, args ...string) *bigquery.Client {
	t.Helper()
	ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
	defer cancel()
	args = append([]string{"--host", "127.0.0.1", "--http-port", "0", `--project={"projectId":"test"}`}, args...)
	p, err := serverprocess.StartEmulator(ctx, binary(t), "", args...)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := p.Stop(); err != nil {
			t.Error(err)
		}
	})
	client, err := bigquery.NewClient(t.Context(), "test", option.WithEndpoint(p.URL), option.WithoutAuthentication())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { client.Close() })
	return client
}

// The first value of a query that calls a FLOAT64 JavaScript UDF with `body`.
func call(t *testing.T, client *bigquery.Client, body string) (bigquery.Value, error) {
	t.Helper()
	ctx, cancel := context.WithTimeout(t.Context(), time.Minute)
	defer cancel()
	it, err := client.Query(`CREATE TEMP FUNCTION f() RETURNS FLOAT64 LANGUAGE js AS r"""` + body + `""";
		SELECT f()`).Read(ctx)
	if err != nil {
		return nil, err
	}
	var row []bigquery.Value
	if err := it.Next(&row); err != nil {
		if errors.Is(err, iterator.Done) {
			t.Fatal("no rows")
		}
		return nil, err
	}
	return row[0], nil
}

func TestTimeout(t *testing.T) {
	client := newClient(t, "--javascript-timeout-ms=100")
	if value, err := call(t, client, "return 1;"); err != nil || value != 1.0 {
		t.Fatalf("call within the limit: %v, %v", value, err)
	}
	// The interruption is not an exception the UDF can catch.
	for _, body := range []string{"while (true) {}", "try { while (true) {} } catch (e) {} return 1;"} {
		t.Run(body, func(t *testing.T) {
			if value, err := call(t, client, body); err == nil {
				t.Fatalf("expected a timeout, got %v", value)
			}
		})
	}
}

func TestMemory(t *testing.T) {
	// About 32 MiB of array elements: more than the configured limit and less than the default.
	const body = "const a = []; for (let i = 0; i < 2000; i++) a.push(new Array(1000).fill(1)); return a.length;"
	if value, err := call(t, newClient(t), body); err != nil || value != 2000.0 {
		t.Fatalf("call within the default limit: %v, %v", value, err)
	}
	if value, err := call(t, newClient(t, "--javascript-memory-mib", "16"), body); err == nil {
		t.Fatalf("expected the memory limit to fail the call, got %v", value)
	}
}

func TestInvalidLimits(t *testing.T) {
	for _, flag := range []string{"--javascript-timeout-ms", "--javascript-memory-mib"} {
		for _, value := range []string{"0", "-1", "1.5", "abc", "1x", "", "4294967296"} {
			t.Run(flag+" "+value, func(t *testing.T) { rejected(t, flag, value) })
			t.Run(flag+"="+value, func(t *testing.T) { rejected(t, flag+"="+value) })
		}
		t.Run(flag, func(t *testing.T) { rejected(t, flag) })
	}
}

func rejected(t *testing.T, args ...string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
	defer cancel()
	output, err := exec.CommandContext(ctx, binary(t), append([]string{"serve", "--host", "127.0.0.1", "--http-port", "0"}, args...)...).CombinedOutput()
	var exit *exec.ExitError
	if ctx.Err() != nil || !errors.As(err, &exit) || exit.ExitCode() != 2 || strings.Contains(string(output), "listening on") {
		t.Fatalf("expected usage error: %v\n%s", err, output)
	}
}
