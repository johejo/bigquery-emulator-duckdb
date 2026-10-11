package emulatorprocess

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"slices"
	"strings"
	"syscall"
	"testing"
	"time"
)

func init() {
	// Start chooses an ephemeral gRPC listener; the helper models that emulator flag.
	flag.Int("grpc-port", 0, "helper gRPC port")
}

// Re-execute the test binary to exercise real subprocess startup and termination.
func TestProcessHelper(t *testing.T) {
	separator := slices.Index(os.Args, "--")
	if separator < 0 {
		return
	}
	switch os.Args[separator+1] {
	case "ready":
		stopped := make(chan os.Signal, 1)
		signal.Notify(stopped, syscall.SIGTERM)
		fmt.Fprintln(os.Stderr, "BigQuery Storage gRPC listening on 127.0.0.1:1235")
		fmt.Fprint(os.Stderr, "diagnostic before startup\nbigquery-emulator-duckdb listening ")
		fmt.Fprintln(os.Stderr, "on http://127.0.0.1:1234")
		<-stopped
		os.Exit(0)
	case "failure":
		fmt.Fprintln(os.Stderr, "invalid configuration")
		os.Exit(7)
	case "hang":
		fmt.Fprintln(os.Stderr, "still starting")
		time.Sleep(time.Minute)
		os.Exit(0)
	}
}

func TestStartAndStop(t *testing.T) {
	ctx, cancel := context.WithTimeout(t.Context(), 10*time.Second)
	defer cancel()
	p, err := Start(ctx, os.Args[0], t.TempDir(), "-test.run=^TestProcessHelper$", "--", "ready")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = p.Stop() })
	if p.URL != "http://127.0.0.1:1234" || p.GRPC != "127.0.0.1:1235" {
		t.Fatalf("URL = %q, GRPC = %q", p.URL, p.GRPC)
	}
	for range 2 {
		if err := p.Stop(); err != nil {
			t.Fatal(err)
		}
	}
}

func TestStartupFailureIncludesStderr(t *testing.T) {
	ctx, cancel := context.WithTimeout(t.Context(), 10*time.Second)
	defer cancel()
	p, err := Start(ctx, os.Args[0], "", "-test.run=^TestProcessHelper$", "--", "failure")
	if p != nil || err == nil || !strings.Contains(err.Error(), "invalid configuration") || !strings.Contains(err.Error(), "exit status 7") {
		t.Fatalf("Start = %v, %v", p, err)
	}
}

func TestStartupTimeout(t *testing.T) {
	ctx, cancel := context.WithTimeout(t.Context(), 200*time.Millisecond)
	defer cancel()
	p, err := Start(ctx, os.Args[0], "", "-test.run=^TestProcessHelper$", "--", "hang")
	if p != nil || !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("Start = %v, %v", p, err)
	}
}
