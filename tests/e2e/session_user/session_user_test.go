package sessionuser

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"

	"github.com/johejo/bigquery-emulator-duckdb/internal/emulatorprocess"
)

func binary(t *testing.T) string {
	t.Helper()
	path := os.Getenv("BQ_EMULATOR_BINARY")
	if path == "" {
		t.Skip("BQ_EMULATOR_BINARY is not set; run just e2e")
	}
	return path
}

func TestEmptyIdentity(t *testing.T) {
	for _, args := range [][]string{{"--session-user", ""}, {"--session-user="}, {"--session-user"}} {
		t.Run(strings.Join(args, " "), func(t *testing.T) {
			ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
			defer cancel()
			output, err := exec.CommandContext(ctx, binary(t), args...).CombinedOutput()
			var exit *exec.ExitError
			if ctx.Err() != nil || !errors.As(err, &exit) || strings.Contains(string(output), "listening on") {
				t.Fatalf("expected startup rejection: %v\n%s", err, output)
			}
		})
	}
}

func TestUnconfigured(t *testing.T) {
	ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
	defer cancel()
	p, err := emulatorprocess.Start(ctx, binary(t), "", "--host", "127.0.0.1", "--port", "0", `--project={"projectId":"test"}`)
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if err := p.Stop(); err != nil {
			t.Error(err)
		}
	}()
	cmd := exec.CommandContext(t.Context(), "runn", "run", "unconfigured.yml")
	cmd.Env = append(os.Environ(), "SESSION_USER_API="+p.URL)
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("runn: %v\n%s", err, output)
	}
}

func TestConfigured(t *testing.T) {
	// Treat identities as data, including quotes and Unicode; do not interpolate them into SQL.
	identity := "principal://example/subject/a'雪"
	for _, args := range [][]string{{"--session-user", identity}, {"--session-user=" + identity}} {
		t.Run(args[0], func(t *testing.T) {
			ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
			defer cancel()
			args = append([]string{"--host", "127.0.0.1", "--port", "0", `--project={"projectId":"one"}`, `--project={"projectId":"two"}`}, args...)
			p, err := emulatorprocess.Start(ctx, binary(t), "", args...)
			if err != nil {
				t.Fatal(err)
			}
			defer func() {
				if err := p.Stop(); err != nil {
					t.Error(err)
				}
			}()
			cmd := exec.CommandContext(t.Context(), "runn", "run", "configured.yml")
			cmd.Env = append(os.Environ(), "SESSION_USER_API="+p.URL, "SESSION_USER_ID="+identity)
			if output, err := cmd.CombinedOutput(); err != nil {
				t.Fatalf("runn: %v\n%s", err, output)
			}
		})
	}
}
