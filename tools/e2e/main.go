// Command e2e runs runn scenarios against the built emulator and fake GCS,
// then restarts the emulator on the same data directory to test persistence.
// Run it through just e2e so that the binary is built first.
package main

import (
	"context"
	"errors"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"github.com/johejo/bigquery-emulator-duckdb/internal/emulatorprocess"
)

func main() {
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	if err := run(ctx, os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

// command also terminates runn's exec-runner descendants when interrupted.
func command(ctx context.Context, binary string, args ...string) *exec.Cmd {
	cmd := exec.CommandContext(ctx, binary, args...)
	cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	cmd.Cancel = func() error { return syscall.Kill(-cmd.Process.Pid, syscall.SIGTERM) }
	cmd.WaitDelay = 15 * time.Second
	return cmd
}

func runn(ctx context.Context, args ...string) error {
	// A nil stdin connects to the null device, since runn otherwise waits for input.
	return command(ctx, "runn", append([]string{"run", "--scopes", "run:exec"}, args...)...).Run()
}

func startEmulator(ctx context.Context, binary, dataDir string, projects []string, sessionUser string) (*emulatorprocess.Process, error) {
	port := os.Getenv("BQ_EMULATOR_PORT")
	if port == "" {
		port = "0"
	}
	args := []string{"--host", "127.0.0.1", "--port", port, "--data-dir", dataDir}
	if sessionUser != "" {
		args = append(args, "--session-user="+sessionUser)
	}
	for _, project := range projects {
		args = append(args, "--project="+project)
	}
	startup, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	p, err := emulatorprocess.Start(startup, binary, "", args...)
	if err != nil {
		return nil, err
	}
	if err := os.Setenv("BQ_EMULATOR_API", p.URL); err != nil {
		_ = p.Stop()
		return nil, err
	}
	return p, nil
}

func startGCS(ctx context.Context) (host string, stop func(), err error) {
	// fake-gcs-server has no emulator-style startup announcement for port 0.
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return "", nil, err
	}
	port := listener.Addr().(*net.TCPAddr).Port
	if err := listener.Close(); err != nil {
		return "", nil, err
	}
	gcsCtx, cancel := context.WithCancel(ctx)
	cmd := command(gcsCtx, "fake-gcs-server", "--scheme", "http", "--host", "127.0.0.1",
		"--port", fmt.Sprint(port), "--backend", "memory", "--data", "tests/e2e/gcs", "--log-level", "error")
	if err := cmd.Start(); err != nil {
		cancel()
		return "", nil, err
	}
	done := make(chan struct{})
	var waitErr error
	go func() {
		waitErr = cmd.Wait()
		close(done)
	}()
	stop = func() { cancel(); <-done }
	host = fmt.Sprintf("127.0.0.1:%d", port)
	startup, startupCancel := context.WithTimeout(ctx, 15*time.Second)
	defer startupCancel()
	client := &http.Client{Timeout: time.Second}
	defer client.CloseIdleConnections()
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for {
		request, err := http.NewRequestWithContext(startup, http.MethodGet, "http://"+host+"/storage/v1/b", nil)
		if err != nil {
			stop()
			return "", nil, err
		}
		response, err := client.Do(request)
		if err == nil {
			response.Body.Close()
			if response.StatusCode == http.StatusOK {
				return host, stop, nil
			}
		}
		select {
		case <-startup.Done():
			stop()
			return "", nil, fmt.Errorf("waiting for fake GCS: %w", startup.Err())
		case <-done:
			cancel()
			return "", nil, fmt.Errorf("fake GCS exited before becoming ready: %v", waitErr)
		case <-ticker.C:
		}
	}
}

func run(ctx context.Context, args []string) (err error) {
	binary, err := filepath.Abs("bazel-bin/bigquery-emulator-duckdb")
	if err != nil {
		return err
	}
	if err := os.Setenv("BQ_EMULATOR_BINARY", binary); err != nil {
		return err
	}
	tmp, err := os.MkdirTemp("", "bigquery-e2e-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmp)
	filesDir, dataDir := filepath.Join(tmp, "files"), filepath.Join(tmp, "data")
	for _, dir := range []string{filesDir, dataDir} {
		if err := os.Mkdir(dir, 0700); err != nil {
			return err
		}
	}
	if err := os.Setenv("E2E_TMP_DIR", filesDir); err != nil {
		return err
	}
	host, stopGCS, err := startGCS(ctx)
	if err != nil {
		return err
	}
	defer stopGCS()
	if err := os.Setenv("STORAGE_EMULATOR_HOST", host); err != nil {
		return err
	}
	projectData, err := os.ReadFile("tests/e2e/projects.jsonl")
	if err != nil {
		return err
	}
	p, err := startEmulator(ctx, binary, dataDir, strings.Split(strings.TrimSpace(string(projectData)), "\n"), "jdoe@example.com")
	if err != nil {
		return err
	}
	defer func() {
		if p != nil {
			err = errors.Join(err, p.Stop())
		}
	}()
	// Runbooks run concurrently, each on datasets of its own. Expand the glob
	// here because exec.Command does not invoke a shell.
	books, err := filepath.Glob("tests/e2e/*.yml")
	if err != nil {
		return err
	}
	if err := runn(ctx, append(append([]string{"--concurrent", "on"}, args...), books...)...); err != nil {
		return err
	}
	if err := runn(ctx, "tests/e2e/restart/before.yml"); err != nil {
		return err
	}
	if err := p.Stop(); err != nil {
		return err
	}
	p, err = startEmulator(ctx, binary, dataDir, nil, "principal://iam.googleapis.com/projects/123/locations/global/workloadIdentityPools/test/subject/alice")
	if err != nil {
		return err
	}
	if err := runn(ctx, "tests/e2e/restart/after.yml"); err != nil {
		return err
	}
	if err := p.Stop(); err != nil {
		return err
	}
	p, err = startEmulator(ctx, binary, dataDir, nil, "")
	if err != nil {
		return err
	}
	return runn(ctx, "tests/e2e/restart/unconfigured.yml")
}
