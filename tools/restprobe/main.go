// Command restprobe probes the emulator through its REST API and prints the Markdown reports
// docs/sql.md, docs/options.md, docs/api.md and docs/functions.md, and writes the function type
// index and domain pages under OUTPUT_DIR. Each SQL, DDL option and API
// probe starts a fresh emulator from the binary that -emulator names, so that probes neither see
// nor disturb each other's data; function calls share a few emulators and their graph fixture. Probes
// run in parallel.
//
//	restprobe -emulator BINARY sql FEATURES_FILE
//	restprobe -emulator BINARY options CATALOG
//	restprobe -emulator BINARY api DISCOVERY_FILE METHODS_FILE
//	restprobe -emulator BINARY functions FUNCTION_PROBE FUNCTIONS_FILE HINTS_FILE
//	restprobe -emulator BINARY [-check] function-types CATALOG FUNCTIONS_FILE OUTPUT_DIR
//
// FUNCTION_PROBE is //:function_probe, which builds the calls. CATALOG is tools/ddl_options.json.
package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"runtime"
	"strings"
	"sync"
	"time"

	"github.com/johejo/bigquery-emulator-duckdb/internal/serverprocess"
)

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

func startEmulator(binary string, args ...string) (*serverprocess.Emulator, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	return serverprocess.StartEmulator(ctx, binary, "", append([]string{"--host", "127.0.0.1", "--http-port", "0",
		"--project", `{"projectId":"test","numericId":"123456789012"}`}, args...)...)
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
	check := flag.Bool("check", false, "check function type report files without writing them")
	flag.Usage = func() {
		fmt.Fprintln(os.Stderr, "usage: restprobe -emulator BINARY sql FEATURES_FILE")
		fmt.Fprintln(os.Stderr, "       restprobe -emulator BINARY options CATALOG")
		fmt.Fprintln(os.Stderr, "       restprobe -emulator BINARY api DISCOVERY_FILE METHODS_FILE")
		fmt.Fprintln(os.Stderr, "       restprobe -emulator BINARY functions FUNCTION_PROBE FUNCTIONS_FILE HINTS_FILE")
		fmt.Fprintln(os.Stderr, "       restprobe -emulator BINARY [-check] function-types CATALOG FUNCTIONS_FILE OUTPUT_DIR")
	}
	flag.Parse()
	args := flag.Args()
	var err error
	switch {
	case *check && (len(args) == 0 || args[0] != "function-types"):
		err = fmt.Errorf("-check is only supported for function-types")
	case *binary != "" && len(args) == 2 && args[0] == "sql":
		err = probeSQL(os.Stdout, *binary, args[1])
	case *binary != "" && len(args) == 2 && args[0] == "options":
		err = probeOptions(os.Stdout, *binary, args[1])
	case *binary != "" && len(args) == 3 && args[0] == "api":
		err = probeAPI(os.Stdout, *binary, args[1], args[2])
	case *binary != "" && len(args) == 4 && args[0] == "function-types":
		err = probeFunctionTypes(*binary, args[1], args[2], args[3], *check)
	case *binary != "" && len(args) == 4 && args[0] == "functions":
		err = probeFunctions(os.Stdout, *binary, args[1], args[2:])
	default:
		flag.Usage()
		os.Exit(2)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
