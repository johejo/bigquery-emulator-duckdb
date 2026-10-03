// Command restprobe probes the emulator through its REST API and prints the Markdown reports
// docs/sql.md and docs/api.md. Each probe starts a fresh emulator from the binary that -emulator
// names, so that probes neither see nor disturb each other's data, and probes run in parallel.
//
//	restprobe -emulator BINARY sql FEATURES_FILE
//	restprobe -emulator BINARY api DISCOVERY_FILE METHODS_FILE
package main

import (
	"bufio"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"runtime"
	"sort"
	"strings"
	"sync"
)

type outcome int

const (
	runs outcome = iota
	broken
	unsupported
	untested
)

type probe struct {
	outcome outcome
	detail  string
}

type line struct {
	number          int
	directive, rest string
}

// readLines returns the lines of path that are neither blank nor comments, split into the first
// word and the rest.
func readLines(path string) ([]line, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var lines []line
	for i, text := range strings.Split(strings.TrimSuffix(string(data), "\n"), "\n") {
		if text == "" || strings.HasPrefix(text, "#") {
			continue
		}
		directive, rest, _ := strings.Cut(text, " ")
		rest = strings.TrimLeft(rest, " \t")
		if rest == "" {
			return nil, fmt.Errorf("%s:%d: cannot parse %s", path, i+1, text)
		}
		lines = append(lines, line{i + 1, directive, rest})
	}
	return lines, nil
}

// splitNamed splits "NAME | TEXT" at the first separator.
func splitNamed(text string) (name, value string, ok bool) {
	name, value, ok = strings.Cut(text, " | ")
	return name, value, ok && name != "" && value != ""
}

func firstLine(text string) string {
	first, _, _ := strings.Cut(text, "\n")
	return first
}

// escape escapes the pipes that would end a Markdown table cell.
func escape(text string) string { return strings.ReplaceAll(text, "|", `\|`) }

// status is Supported, Partial, Unsupported, Broken or Untested, from how many probes had each
// outcome.
func status(counts map[outcome]int) string {
	tested := counts[runs] + counts[unsupported] + counts[broken]
	switch {
	case counts[broken] > 0:
		return "Broken"
	case tested == 0:
		return "Untested"
	case counts[runs] == tested:
		return "Supported"
	case counts[runs] == 0:
		return "Unsupported"
	}
	return "Partial"
}

// writeTotals prints how many rows have each status, in the order of the status names.
func writeTotals(w io.Writer, heading string, totals map[string]int) {
	fmt.Fprintf(w, "| Status | %s |\n| --- | --- |\n", heading)
	for _, name := range sortedNames(totals) {
		fmt.Fprintf(w, "| %s | %d |\n", name, totals[name])
	}
}

// sortedNames returns the keys of entries in order.
func sortedNames[V any](entries map[string]V) []string {
	names := make([]string, 0, len(entries))
	for name := range entries {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

// emulator is a running emulator process that listens on a port of its own.
type emulator struct {
	cmd *exec.Cmd
	url string
}

func startEmulator(binary string) (*emulator, error) {
	cmd := exec.Command(binary, "--host", "127.0.0.1", "--port", "0")
	stderr, err := cmd.StderrPipe()
	if err != nil {
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	const listening = "bigquery-emulator-duckdb listening on "
	scanner := bufio.NewScanner(stderr)
	for scanner.Scan() {
		if url, ok := strings.CutPrefix(scanner.Text(), listening); ok {
			// Keep draining stderr so that the emulator never blocks on it.
			go io.Copy(io.Discard, stderr)
			return &emulator{cmd, url}, nil
		}
	}
	cmd.Process.Kill()
	cmd.Wait()
	return nil, fmt.Errorf("%s exited before listening", binary)
}

func (e *emulator) stop() {
	e.cmd.Process.Kill()
	e.cmd.Wait()
}

// parallel calls run(i) for each i below n, as many at a time as there are CPUs, and returns the
// first error.
func parallel(n int, run func(i int) error) error {
	var wg sync.WaitGroup
	var once sync.Once
	var first error
	limit := make(chan struct{}, runtime.NumCPU())
	for i := range n {
		wg.Add(1)
		limit <- struct{}{}
		go func() {
			defer func() { <-limit; wg.Done() }()
			if err := run(i); err != nil {
				once.Do(func() { first = err })
			}
		}()
	}
	wg.Wait()
	return first
}

func main() {
	binary := flag.String("emulator", "", "the emulator binary to probe")
	flag.Usage = func() {
		fmt.Fprintln(os.Stderr, "usage: restprobe -emulator BINARY sql FEATURES_FILE")
		fmt.Fprintln(os.Stderr, "       restprobe -emulator BINARY api DISCOVERY_FILE METHODS_FILE")
	}
	flag.Parse()
	args := flag.Args()
	var err error
	switch {
	case *binary != "" && len(args) == 2 && args[0] == "sql":
		err = probeSQL(os.Stdout, *binary, args[1])
	case *binary != "" && len(args) == 3 && args[0] == "api":
		err = probeAPI(os.Stdout, *binary, args[1], args[2])
	default:
		flag.Usage()
		os.Exit(2)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
