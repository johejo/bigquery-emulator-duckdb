package shell

import (
	"bytes"
	"context"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/creack/pty"
)

// terminal is a shell whose standard input is a terminal; its standard output and error are not.
type terminal struct {
	t      *testing.T
	cmd    *exec.Cmd
	input  *os.File
	mu     sync.Mutex
	stdout bytes.Buffer
	stderr bytes.Buffer
}

func (b *terminal) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.stderr.Write(p)
}

func startTerminal(t *testing.T, args ...string) *terminal {
	t.Helper()
	binary := os.Getenv("BQ_EMULATOR_BINARY")
	if binary == "" {
		t.Skip("BQ_EMULATOR_BINARY is not set; run just e2e")
	}
	ctx, cancel := context.WithTimeout(t.Context(), 30*time.Second)
	t.Cleanup(cancel)
	input, tty, err := pty.Open()
	if err != nil {
		t.Fatal(err)
	}
	term := &terminal{t: t, input: input}
	term.cmd = exec.CommandContext(ctx, binary, append([]string{"shell", project}, args...)...)
	term.cmd.Stdin, term.cmd.Stdout, term.cmd.Stderr = tty, &term.stdout, term
	// The terminal is the shell's controlling terminal, so that ^C sends it SIGINT.
	term.cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true, Setctty: true, Ctty: 0}
	if err := term.cmd.Start(); err != nil {
		t.Fatal(err)
	}
	tty.Close()
	// Discards the terminal's echo of the input.
	go func() { _, _ = io.Copy(io.Discard, input) }()
	t.Cleanup(func() { input.Close() })
	term.waitPrompts(1)
	return term
}

func (term *terminal) prompts() int {
	term.mu.Lock()
	defer term.mu.Unlock()
	return strings.Count(term.stderr.String(), "> ")
}

// waitPrompts waits until the shell has prompted `count` times in all.
func (term *terminal) waitPrompts(count int) {
	term.t.Helper()
	deadline := time.Now().Add(20 * time.Second)
	for term.prompts() < count {
		if time.Now().After(deadline) {
			term.t.Fatalf("waiting for prompt %d; stderr:\n%s", count, term.errors())
		}
		time.Sleep(10 * time.Millisecond)
	}
}

// send types `text` and waits for the shell to read it: one prompt follows each line, or the
// discarded entry after ^C.
func (term *terminal) send(text string) {
	term.t.Helper()
	before := term.prompts()
	if _, err := term.input.WriteString(text); err != nil {
		term.t.Fatal(err)
	}
	term.waitPrompts(before + strings.Count(text, "\n") + strings.Count(text, "\x03"))
}

func (term *terminal) errors() string {
	term.mu.Lock()
	defer term.mu.Unlock()
	return term.stderr.String()
}

// close ends the input with ^D and returns standard output once the shell exits.
func (term *terminal) close() string {
	term.t.Helper()
	if _, err := term.input.WriteString("\x04"); err != nil {
		term.t.Fatal(err)
	}
	if err := term.cmd.Wait(); err != nil {
		term.t.Fatalf("%v; stderr:\n%s", err, term.errors())
	}
	return term.stdout.String()
}

// Each testdata/terminal/NAME.sql is typed a line at a time, and the shell writes NAME.golden.
func TestTerminalEntries(t *testing.T) {
	inputs, err := filepath.Glob("testdata/terminal/*.sql")
	if err != nil {
		t.Fatal(err)
	}
	for _, input := range inputs {
		name := strings.TrimSuffix(filepath.Base(input), ".sql")
		t.Run(name, func(t *testing.T) {
			term := startTerminal(t)
			for _, line := range strings.SplitAfter(golden(t, input), "\n") {
				if line != "" {
					term.send(line)
				}
			}
			want := golden(t, strings.TrimSuffix(input, ".sql")+".golden")
			if got := term.close(); got != want {
				t.Errorf("got %q, want %q; stderr:\n%s", got, want, term.errors())
			}
		})
	}
}

// Each entry is its own job, as in BigQuery without sessions, and a failed one does not end the
// shell.
func TestTerminalJobsShareNoVariables(t *testing.T) {
	term := startTerminal(t)
	term.send("DECLARE v INT64 DEFAULT 1;\n")
	term.send("SELECT v;\n")
	term.send("SELEC 1;\n")
	term.send("SELECT 1 AS a;\n")
	if got := term.close(); got != `{"a":"1"}`+"\n" {
		t.Errorf("got %q", got)
	}
	if errors := strings.Count(term.errors(), "Error: "); errors != 2 {
		t.Errorf("got %d errors; stderr:\n%s", errors, term.errors())
	}
}

func TestTerminalInterruptDiscardsEntry(t *testing.T) {
	term := startTerminal(t)
	term.send("SELECT 1 AS a\n")
	term.send("\x03")
	term.send("SELECT 2 AS b;\n")
	if got := term.close(); got != `{"b":"2"}`+"\n" {
		t.Errorf("got %q; stderr:\n%s", got, term.errors())
	}
}

func TestTerminalCommands(t *testing.T) {
	dir := t.TempDir()
	expect(t, run(t, "CREATE SCHEMA ds;\nCREATE TABLE ds.t AS SELECT 1 AS x;\n", project, "--data-dir", dir), result{})
	term := startTerminal(t, "--data-dir", dir)
	term.send(".format csv\n")
	term.send("SELECT 1 AS a;\n")
	term.send("\\format jsonl\n")
	term.send(".use test.ds\n")
	term.send("SELECT x FROM t;\n")
	term.send(".use missing\n")
	term.send(".nothing\n")
	term.send(".use test\n")
	term.send("SELECT x FROM t;\n")
	if got, want := term.close(), "a\r\n1\r\n"+`{"x":"1"}`+"\n"; got != want {
		t.Errorf("got %q, want %q", got, want)
	}
	if errors := strings.Count(term.errors(), "Error: "); errors != 3 {
		t.Errorf("got %d errors; stderr:\n%s", errors, term.errors())
	}
	if !strings.Contains(term.errors(), "test.ds> ") {
		t.Errorf("the prompt names no dataset; stderr:\n%s", term.errors())
	}
}

func TestTerminalQuit(t *testing.T) {
	term := startTerminal(t)
	if _, err := term.input.WriteString(".quit\n"); err != nil {
		t.Fatal(err)
	}
	if err := term.cmd.Wait(); err != nil {
		t.Fatal(err)
	}
}
