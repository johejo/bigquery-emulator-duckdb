package projects

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
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

func server(t *testing.T, dir string, args ...string) *emulatorprocess.Process {
	t.Helper()
	ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
	defer cancel()
	p, err := emulatorprocess.Start(ctx, binary(t), dir,
		append([]string{"--host", "127.0.0.1", "--port", "0"}, args...)...)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { stop(t, p) })
	return p
}

func stop(t *testing.T, p *emulatorprocess.Process) {
	t.Helper()
	if err := p.Stop(); err != nil {
		t.Error(err)
	}
}

func runbook(t *testing.T, url, name string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(t.Context(), 90*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "runn", "run", name)
	cmd.Env = append(os.Environ(), "PROJECTS_API="+url)
	// A nil stdin connects to the null device; runn must not wait for input.
	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("runn %s: %v\n%s", name, err, output)
	}
}

func rejected(t *testing.T, args ...string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
	defer cancel()
	output, err := exec.CommandContext(ctx, binary(t),
		append([]string{"--host", "127.0.0.1", "--port", "0"}, args...)...).CombinedOutput()
	var exit *exec.ExitError
	if ctx.Err() != nil || !errors.As(err, &exit) || strings.Contains(string(output), "listening on") {
		t.Fatalf("expected startup rejection for %q: %v\n%s", args, err, output)
	}
}

func write(t *testing.T, path, contents string) {
	t.Helper()
	if err := os.WriteFile(path, []byte(contents), 0600); err != nil {
		t.Fatal(err)
	}
}

func read(t *testing.T, path string) []byte {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func TestEmptyProjects(t *testing.T) {
	p := server(t, "")
	runbook(t, p.URL, "rest_empty.yml")
}

func TestProjectPersistence(t *testing.T) {
	binary(t)
	dir := t.TempDir()
	dataDir := filepath.Join(dir, "data")
	projectFile := filepath.Join(dir, "project.json")
	write(t, projectFile, `{"projectId":"z-configured","numericId":"123456789012","friendlyName":""}`)
	args := []string{"--data-dir", dataDir, "--project=@project.json"}
	for i := range 51 {
		args = append(args, fmt.Sprintf(`--project={"projectId":"p%02d"}`, i))
	}
	p := server(t, dir, args...)
	runbook(t, p.URL, "rest_projects.yml")
	stop(t, p)

	p = server(t, "", "--data-dir", dataDir)
	runbook(t, p.URL, "rest_restore.yml")
	stop(t, p)

	p = server(t, "", "--data-dir", dataDir,
		`--project={"projectId":"z-configured","friendlyName":"Updated"}`)
	runbook(t, p.URL, "rest_update.yml")
	stop(t, p)

	// Invalid startup configurations must not alter persisted registrations.
	registrations := filepath.Join(dataDir, "projects.json")
	saved := read(t, registrations)
	rejected(t, "--data-dir", dataDir,
		`--project={"projectId":"a","numericId":"42"}`,
		`--project={"projectId":"b","numericId":"42"}`)
	if !bytes.Equal(read(t, registrations), saved) {
		t.Fatal("invalid configuration changed persisted registrations")
	}
	bad := filepath.Join(dir, "bad.json")
	write(t, bad, `{"projectId":"bad"} trailing`)
	rejected(t, "--project=@"+bad)
	rejected(t, "--project="+projectFile)
	rejected(t, "--project=@"+filepath.Join(dir, "missing.json"))
}

func TestInvalidProjects(t *testing.T) {
	binary(t)
	for _, value := range []string{
		"[]", "null", "{}", `{"projectId":""}`, `{"projectId":"a/b"}`,
		`{"projectId":"p","other":"x"}`, `{"projectId":123}`,
		`{"projectId":"p","friendlyName":null}`,
		`{"projectId":"p","numericId":123}`,
		`{"projectId":"p","numericId":"0"}`,
		`{"projectId":"p","numericId":"01"}`,
		`{"projectId":"p","numericId":"18446744073709551616"}`,
	} {
		t.Run(value, func(t *testing.T) { rejected(t, "--project="+value) })
	}
	rejected(t, `--project={"projectId":"p"}`, `--project={"projectId":"p"}`)
	rejected(t, `--project={"projectId":"p","numericId":"42"}`, `--project={"projectId":"42"}`)
	rejected(t, "--project=@-")
	rejected(t, "--project=@")
}
