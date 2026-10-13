package shell

import (
	"bytes"
	"context"
	"errors"
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"
)

const project = `--project={"projectId":"test"}`

type result struct {
	stdout, stderr string
	code           int
}

// run starts the shell with stdin as its standard input, which is not a terminal.
func run(t *testing.T, stdin string, args ...string) result {
	t.Helper()
	binary := os.Getenv("BQ_EMULATOR_BINARY")
	if binary == "" {
		t.Skip("BQ_EMULATOR_BINARY is not set; run just e2e")
	}
	ctx, cancel := context.WithTimeout(t.Context(), 30*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, binary, append([]string{"shell"}, args...)...)
	cmd.Stdin = strings.NewReader(stdin)
	var stdout, stderr bytes.Buffer
	cmd.Stdout, cmd.Stderr = &stdout, &stderr
	err := cmd.Run()
	var exit *exec.ExitError
	if err != nil && !errors.As(err, &exit) {
		t.Fatal(err)
	}
	return result{stdout.String(), stderr.String(), cmd.ProcessState.ExitCode()}
}

func expect(t *testing.T, got result, want result) {
	t.Helper()
	if got != want {
		t.Errorf("got  %+v\nwant %+v", got, want)
	}
}

// Input that is not a terminal runs as one script, whose result is its last statement's.
func TestStdinRunsOneScript(t *testing.T) {
	expect(t, run(t, `
CREATE SCHEMA ds;
CREATE TABLE ds.t (x INT64, s STRING);
INSERT ds.t VALUES (2, NULL), (1, 'a');
SELECT x, s FROM ds.t ORDER BY x;
`, project), result{stdout: "{\"x\":\"1\",\"s\":\"a\"}\n{\"x\":\"2\",\"s\":null}\n"})
}

// Variables live as long as the script, which is all of standard input.
func TestStdinKeepsScriptVariables(t *testing.T) {
	expect(t, run(t, "DECLARE x INT64 DEFAULT 1;\nSELECT x + 1 AS y;\n", project),
		result{stdout: "{\"y\":\"2\"}\n"})
}

// Values keep the REST API's encoding, such as INT64 as a string, in every format; columns keep
// the schema's order. In pretty, columns align by code point and NULL differs from an empty string.
func TestExecuteFormats(t *testing.T) {
	query := golden(t, "testdata/formats/query.sql")
	for _, format := range []string{"pretty", "json", "jsonl", "csv"} {
		t.Run(format, func(t *testing.T) {
			expect(t, run(t, "", project, "--format", format, "-e", query),
				result{stdout: golden(t, "testdata/formats/"+format+".golden")})
		})
	}
}

func golden(t *testing.T, path string) string {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return string(data)
}

func TestEmptyResultAndInput(t *testing.T) {
	expect(t, run(t, "", project, "-e", "SELECT 1 AS n LIMIT 0"), result{})
	expect(t, run(t, "", project, "--format", "csv", "-e", "SELECT 1 AS n LIMIT 0"), result{stdout: "n\r\n"})
	expect(t, run(t, " \n", project), result{})
}

// A DML statement's affected rows go to standard error, leaving standard output to results.
func TestDmlReportsAffectedRows(t *testing.T) {
	dir := t.TempDir()
	expect(t, run(t, "CREATE SCHEMA ds;\nCREATE TABLE ds.t (x INT64);\n", project, "--data-dir", dir), result{})
	expect(t, run(t, "", "--data-dir", dir, "-e", "INSERT ds.t VALUES (1), (2)"),
		result{stderr: "2 rows affected\n"})
}

func TestErrorExitsWithFailure(t *testing.T) {
	got := run(t, "SELECT * FROM missing.t;\n", project)
	if got.code != 1 || got.stdout != "" || !strings.HasPrefix(got.stderr, "Error: ") {
		t.Errorf("got %+v", got)
	}
}

func TestProjectSelection(t *testing.T) {
	dir := t.TempDir()
	other := `--project={"projectId":"other"}`
	got := run(t, "", project, other, "--data-dir", dir, "-e", "SELECT 1")
	if got.code != 2 || got.stdout != "" {
		t.Errorf("without --project-id: got %+v", got)
	}
	expect(t, run(t, "CREATE SCHEMA ds;\nCREATE TABLE ds.t AS SELECT 'other' AS p;\n",
		"--data-dir", dir, "--project-id", "other"), result{})
	expect(t, run(t, "", "--data-dir", dir, "--project-id", "other", "-e", "SELECT p FROM ds.t"),
		result{stdout: "{\"p\":\"other\"}\n"})
	if got := run(t, "", "--data-dir", dir, "--project-id", "test", "-e", "SELECT p FROM ds.t"); got.code != 1 {
		t.Errorf("another project's table: got %+v", got)
	}
	if got := run(t, "", "--data-dir", dir, "--project-id", "missing", "-e", "SELECT 1"); got.code != 2 || got.stdout != "" {
		t.Errorf("unregistered project: got %+v", got)
	}
}

func TestInvalidFormat(t *testing.T) {
	got := run(t, "", project, "--format", "yaml", "-e", "SELECT 1")
	if got.code != 2 || got.stdout != "" {
		t.Errorf("got %+v", got)
	}
}

func TestDataDirPersists(t *testing.T) {
	dir := t.TempDir()
	expect(t, run(t, "CREATE SCHEMA ds;\nCREATE TABLE ds.t AS SELECT 1 AS x;\n", project, "--data-dir", dir), result{})
	expect(t, run(t, "", "--data-dir", dir, "--dataset-id", "ds", "-e", "SELECT x FROM t"),
		result{stdout: "{\"x\":\"1\"}\n"})
}
