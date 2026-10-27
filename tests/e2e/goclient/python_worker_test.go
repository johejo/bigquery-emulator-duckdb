package goclient

import (
	"context"
	"fmt"
	"os"
	"runtime"
	"strings"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"github.com/johejo/bigquery-emulator-duckdb/internal/emulatorprocess"
	"google.golang.org/api/option"
)

// These checks exercise local worker failures and resource limits, rather than BigQuery's
// Python conversion rules, which belong in the scalar cases.
func TestPythonWorkerIsolation(t *testing.T) {
	t.Parallel()
	binary := os.Getenv("BQ_EMULATOR_BINARY")
	if binary == "" {
		t.Skip("BQ_EMULATOR_BINARY is not set; run just e2e")
	}
	memoryError := "MemoryError"
	if runtime.GOOS == "darwin" {
		memoryError = "worker memory limit exceeded"
	}
	for _, tc := range []struct {
		name    string
		body    string
		err     string
		timeout time.Duration
	}{
		{"loop", "def f():\n    while True: pass", "timed out", time.Second},
		{"blocking_native", "import time\ndef f(): time.sleep(3600)", "timed out", time.Second},
		{"crash", "import os\ndef f(): os._exit(7)", "worker exited", 10 * time.Second},
		{"exception", "def f(): raise ValueError('worker test')", "ValueError: worker test", 10 * time.Second},
		{"memory", "def f():\n    blocks = []\n    while True: blocks.append(b'x' * (1024 * 1024))", memoryError, 10 * time.Second},
		// Slow progress must still exercise the memory limit, rather than race the
		// short deadline used by the timeout cases. Sleep makes this reproducible
		// without depending on the host's CPU load or scheduling.
		{"memory_after_delay", "import time\ndef f():\n    time.sleep(2)\n    blocks = []\n    while True: blocks.append(b'x' * (1024 * 1024))", memoryError, 10 * time.Second},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			// Only timeout cases use a short deadline. Other failures need enough
			// time for interpreter startup and execution on a busy host. Each case
			// gets its own server so its limits cannot affect another case.
			client := pythonWorkerClient(t, binary, tc.timeout, 64)
			sql := fmt.Sprintf(`CREATE FUNCTION ds.%s() RETURNS INT64 LANGUAGE python
				OPTIONS(runtime_version='python-3.11', entry_point='f') AS r'''%s'''; SELECT ds.%s()`, tc.name, tc.body, tc.name)
			start := time.Now()
			_, _, err := runScript(t, client, "", sql)
			if err == nil || !strings.Contains(err.Error(), tc.err) {
				t.Fatalf("got %v, want %s", err, tc.err)
			}
			if elapsed := time.Since(start); elapsed > tc.timeout+10*time.Second {
				t.Fatalf("worker failure took %s", elapsed)
			}
			// A failed Python worker must not damage the server or prevent a fresh worker
			// from executing another Python query.
			sql = fmt.Sprintf(`CREATE FUNCTION ds.%s_recovery(x INT64) RETURNS INT64 LANGUAGE python
				OPTIONS(runtime_version='python-3.11', entry_point='f') AS 'def f(x): return x'; SELECT ds.%s_recovery(1)`, tc.name, tc.name)
			if _, _, err = runScript(t, client, "", sql); err != nil {
				t.Fatalf("Python query after worker failure: %v", err)
			}
		})
	}
}

// A batch may take longer than one call's limit, but a stuck later row must still
// fail. These are local execution-limit tests, independent of BigQuery's limits.
func TestPythonWorkerBatchLimits(t *testing.T) {
	t.Parallel()
	binary := os.Getenv("BQ_EMULATOR_BINARY")
	if binary == "" {
		t.Skip("BQ_EMULATOR_BINARY is not set; run just e2e")
	}
	for _, tc := range []struct {
		name string
		body string
		rows int
		want int64
		err  string
	}{
		{"progress", "import time\ndef f():\n    time.sleep(0.25)\n    return 1", 16, 16, ""},
		{"later_timeout", "import time\nn = 0\ndef f():\n    global n\n    n += 1\n    if n == 3: time.sleep(3600)\n    return 1", 16, 0, "timed out"},
		{"stop_on_error", "import os\nn = 0\ndef f():\n    global n\n    n += 1\n    if n == 2: raise ValueError('batch stopped')\n    if n > 2: os._exit(7)\n    return 1", 16, 0, "ValueError: batch stopped"},
		// Together these results exceed one protocol frame's hard limit. Each
		// result fits; batching must not introduce an aggregate output limit.
		{"large_results", "def f(): return 'x' * (1024 * 1024)", 65, 65 * 1024 * 1024, ""},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			client := pythonWorkerClient(t, binary, 3*time.Second, 256)
			resultType, expression := "INT64", "ds.f()"
			if tc.name == "large_results" {
				resultType, expression = "STRING", "BYTE_LENGTH(ds.f())"
			}
			sql := fmt.Sprintf(`CREATE FUNCTION ds.f() RETURNS %s LANGUAGE python
				OPTIONS(runtime_version='python-3.11', entry_point='f') AS r'''%s''';
				SELECT SUM(%s) FROM UNNEST(GENERATE_ARRAY(1, %d))`, resultType, tc.body, expression, tc.rows)
			job, _, err := runScript(t, client, "", sql)
			if tc.err != "" {
				if err == nil || !strings.Contains(err.Error(), tc.err) {
					t.Fatalf("got %v, want %s", err, tc.err)
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			rows, err := job.Read(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			var values []bigquery.Value
			if err := rows.Next(&values); err != nil {
				t.Fatal(err)
			}
			if len(values) != 1 || values[0] != tc.want {
				t.Fatalf("got %v, want %d", values, tc.want)
			}
		})
	}
}

func pythonWorkerClient(t *testing.T, binary string, timeout time.Duration, memoryMiB int) *bigquery.Client {
	t.Helper()
	ctx := t.Context()
	startup, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	process, err := emulatorprocess.Start(startup, binary, "", "--host", "127.0.0.1", "--http-port", "0",
		`--project={"projectId":"python-worker"}`, fmt.Sprintf("--python-timeout-ms=%d", timeout.Milliseconds()),
		fmt.Sprintf("--python-memory-mib=%d", memoryMiB))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := process.Stop(); err != nil {
			t.Error(err)
		}
	})
	client, err := bigquery.NewClient(ctx, "python-worker", option.WithEndpoint(process.URL), option.WithoutAuthentication())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = client.Close() })
	if err := client.Dataset("ds").Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	return client
}
