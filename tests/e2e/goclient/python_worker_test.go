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
	ctx := t.Context()
	startup, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	process, err := emulatorprocess.Start(startup, binary, "", "--host", "127.0.0.1", "--port", "0",
		`--project={"projectId":"python-worker"}`, "--python-timeout-ms=1000", "--python-memory-mib=64")
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
	memoryError := "MemoryError"
	if runtime.GOOS == "darwin" {
		memoryError = "worker memory limit exceeded"
	}
	for _, tc := range []struct {
		name string
		body string
		err  string
	}{
		{"loop", "def f():\n    while True: pass", "timed out"},
		{"blocking_native", "import time\ndef f(): time.sleep(3600)", "timed out"},
		{"crash", "import os\ndef f(): os._exit(7)", "worker exited"},
		{"exception", "def f(): raise ValueError('worker test')", "ValueError: worker test"},
		{"memory", "def f():\n    blocks = []\n    while True: blocks.append(b'x' * (1024 * 1024))", memoryError},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			sql := fmt.Sprintf(`CREATE FUNCTION ds.%s() RETURNS INT64 LANGUAGE python
				OPTIONS(runtime_version='python-3.11', entry_point='f') AS r'''%s'''; SELECT ds.%s()`, tc.name, tc.body, tc.name)
			start := time.Now()
			_, _, err := runScript(t, client, "", sql)
			if err == nil || !strings.Contains(err.Error(), tc.err) {
				t.Fatalf("got %v, want %s", err, tc.err)
			}
			if elapsed := time.Since(start); elapsed > 10*time.Second {
				t.Fatalf("worker failure took %s", elapsed)
			}
			// A failed Python worker must not damage the server or prevent a fresh worker
			// from executing another Python query.
			sql = fmt.Sprintf(`CREATE FUNCTION ds.%s_recovery(x INT64) RETURNS INT64 LANGUAGE python
				OPTIONS(runtime_version='python-3.11', entry_point='f') AS 'def f(x): return x'; SELECT ds.%s_recovery(1)`, tc.name, tc.name)
			if _, _, err := runScript(t, client, "", sql); err != nil {
				t.Fatalf("Python query after worker failure: %v", err)
			}
		})
	}
}
