// Command bazelsummary renders a Bazel Build Event Protocol JSON file as Markdown.
package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
)

type metrics struct {
	Timing struct {
		Wall      int64 `json:"wallTimeInMs,string"`
		Analysis  int64 `json:"analysisPhaseTimeInMs,string"`
		Execution int64 `json:"executionPhaseTimeInMs,string"`
	} `json:"timingMetrics"`
	Actions struct {
		Runners []struct {
			Name  string `json:"name"`
			Count int64  `json:"count"`
		} `json:"runnerCount"`
	} `json:"actionSummary"`
}

type report struct {
	metrics    *metrics
	logs       map[string]string
	incomplete bool
	exit       *exitCode
}

type exitCode struct {
	Name string `json:"name"`
	Code int    `json:"code"`
}

func readReport(r io.Reader) (report, error) {
	result := report{logs: map[string]string{}}
	decoder := json.NewDecoder(r)
	for {
		var event struct {
			Metrics  *metrics `json:"buildMetrics"`
			Finished struct {
				Exit *exitCode `json:"exitCode"`
			} `json:"finished"`
			Logs struct {
				Log []struct {
					Name     string `json:"name"`
					Contents []byte `json:"contents"` // Protobuf JSON encodes bytes as base64.
				} `json:"log"`
			} `json:"buildToolLogs"`
		}
		err := decoder.Decode(&event)
		if errors.Is(err, io.EOF) {
			return result, nil
		}
		if errors.Is(err, io.ErrUnexpectedEOF) {
			result.incomplete = true
			return result, nil
		}
		if err != nil {
			return result, err
		}
		if event.Metrics != nil {
			result.metrics = event.Metrics
		}
		if event.Finished.Exit != nil {
			result.exit = event.Finished.Exit
		}
		for _, log := range event.Logs.Log {
			if log.Contents != nil {
				result.logs[log.Name] = string(log.Contents)
			}
		}
	}
}

func writeReport(w io.Writer, r report) {
	if r.exit != nil {
		fmt.Fprintf(w, "Bazel exit status: %s (%d)\n\n", r.exit.Name, r.exit.Code)
	} else {
		fmt.Fprint(w, "Bazel did not emit a build completion event.\n\n")
	}
	if r.incomplete {
		fmt.Fprint(w, "The JSON report is incomplete; available metrics are shown below.\n\n")
	}
	if r.metrics == nil {
		fmt.Fprint(w, "Bazel did not emit build metrics.\n\n")
		return
	}
	fmt.Fprintln(w, "| Timing | Seconds |\n| --- | ---: |")
	if elapsed, err := strconv.ParseFloat(r.logs["elapsed time"], 64); err == nil {
		fmt.Fprintf(w, "| Elapsed | %.3f |\n", elapsed)
	}
	if path, ok := strings.CutPrefix(r.logs["critical path"], "Critical Path: "); ok {
		seconds, _, _ := strings.Cut(path, "s")
		if value, err := strconv.ParseFloat(seconds, 64); err == nil {
			fmt.Fprintf(w, "| Critical path | %.3f |\n", value)
		}
	}
	// Protobuf JSON omits zero-valued fields.
	fmt.Fprintf(w, "| Wall time | %.3f |\n", float64(r.metrics.Timing.Wall)/1000)
	fmt.Fprintf(w, "| Analysis | %.3f |\n", float64(r.metrics.Timing.Analysis)/1000)
	fmt.Fprintf(w, "| Execution | %.3f |\n\n", float64(r.metrics.Timing.Execution)/1000)
	if r.metrics.Actions.Runners == nil {
		fmt.Fprint(w, "Bazel did not emit process/cache counts.\n\n")
		return
	}
	counts := map[string]int64{"disk cache hit": 0}
	for _, runner := range r.metrics.Actions.Runners {
		counts[runner.Name] = runner.Count
	}
	names := make([]string, 0, len(counts))
	for name := range counts {
		names = append(names, name)
	}
	slices.Sort(names)
	fmt.Fprintln(w, "| Process / cache | Count |\n| --- | ---: |")
	for _, name := range names {
		fmt.Fprintf(w, "| %s | %d |\n", name, counts[name])
	}
	fmt.Fprint(w, "\n`disk cache hit` counts actions reused from the disk cache. Actions reused in memory by the same Bazel server are not included.\n\n")
}

func writeFile(w io.Writer, path string) error {
	title := strings.TrimSuffix(filepath.Base(path), ".json")
	fmt.Fprintf(w, "### Bazel: %s\n\n", title)
	file, err := os.Open(path)
	if err != nil {
		return err
	}
	defer file.Close()
	r, err := readReport(file)
	if err != nil {
		return err
	}
	writeReport(w, r)
	return nil
}

func summarize(w io.Writer, path string) error {
	info, err := os.Stat(path)
	if err != nil {
		return err
	}
	if !info.IsDir() {
		return writeFile(w, path)
	}
	paths, err := filepath.Glob(filepath.Join(path, "*.json"))
	if err != nil {
		return err
	}
	if len(paths) == 0 {
		fmt.Fprint(w, "No Bazel JSON reports were emitted.\n\n")
	}
	for _, path := range paths {
		if err := writeFile(w, path); err != nil {
			return err
		}
	}
	return nil
}

func main() {
	flag.Parse()
	if flag.NArg() != 1 {
		fmt.Fprintln(os.Stderr, "usage: bazelsummary FILE_OR_DIRECTORY")
		os.Exit(2)
	}
	if err := summarize(os.Stdout, flag.Arg(0)); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
