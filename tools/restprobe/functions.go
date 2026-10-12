package main

import (
	"bytes"
	_ "embed"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"runtime"
	"slices"
	"strings"

	"github.com/johejo/bigquery-emulator-duckdb/internal/emulatorprocess"
)

//go:embed functions_preamble.md
var functionsPreamble string

const functionDocs = "https://cloud.google.com/bigquery/docs/reference/standard-sql"

// function is a line that //:function_probe prints: a function, the calls it built for it and the
// probes it settled without running a query.
type function struct {
	Name string `json:"name"`
	// The pages and anchors that document the function.
	Docs           []string `json:"docs"`
	Implementation string   `json:"implementation"`
	Note           string   `json:"note"`
	Queries        []string `json:"queries"`
	Untested       []string `json:"untested"`
	Unsupported    []string `json:"unsupported"`
}

// mainDoc is the page that documents the function itself rather than a variant of it, such as AVG
// over aggregate-dp-functions#dp_avg: the one whose anchor is the function's name.
func mainDoc(f *function) string {
	anchor := "#" + strings.ReplaceAll(strings.ToLower(f.Name), ".", "")
	for _, doc := range f.Docs {
		if strings.HasSuffix(doc, anchor) {
			return doc
		}
	}
	return f.Docs[0]
}

// categories names the pages that document the function, which name its categories.
func categories(f *function) string {
	pages := map[string]bool{}
	for _, doc := range f.Docs {
		page, _, _ := strings.Cut(doc, "#")
		pages[page] = true
	}
	return strings.Join(sortedNames(pages), ", ")
}

// probeFunctions runs the calls that the function probe at path builds from args on emulators
// started from binary. The calls need no data, so each emulator runs many of them.
func probeFunctions(w io.Writer, binary, path string, args []string) error {
	command := exec.Command(path, args...)
	command.Stderr = os.Stderr
	output, err := command.Output()
	if err != nil {
		return fmt.Errorf("%s: %w", path, err)
	}
	var functions []*function
	for line := range bytes.Lines(output) {
		f := &function{}
		if err := json.Unmarshal(line, f); err != nil {
			return fmt.Errorf("%s: %w", path, err)
		}
		if len(f.Docs) == 0 {
			return fmt.Errorf("%s: no docs for %s", path, f.Name)
		}
		functions = append(functions, f)
	}

	type job struct {
		function *function
		sql      string
	}
	var jobs []job
	for _, f := range functions {
		for _, sql := range f.Queries {
			jobs = append(jobs, job{f, sql})
		}
	}
	emulators := make(chan *emulatorprocess.Process, runtime.NumCPU())
	defer func() {
		close(emulators)
		for emulator := range emulators {
			emulator.Stop()
		}
	}()
	if err := parallel(cap(emulators), func(int) error {
		// SESSION_USER needs an identity to run.
		emulator, err := startEmulator(binary, "--session-user", "jdoe@example.com")
		if err == nil {
			emulators <- emulator
		}
		return err
	}); err != nil {
		return err
	}
	probes := make([]probe, len(jobs))
	if err := parallel(len(jobs), func(i int) error {
		emulator := <-emulators
		defer func() { emulators <- emulator }()
		message, failed, err := query(emulator.URL, jobs[i].sql, "")
		if err == nil && failed {
			probes[i] = classify(jobs[i].sql, message)
		}
		return err
	}); err != nil {
		return err
	}
	results := map[*function][]probe{}
	for i, j := range jobs {
		results[j.function] = append(results[j.function], probes[i])
	}

	totals := map[string]int{}
	var body bytes.Buffer
	for _, f := range functions {
		all := results[f]
		for _, detail := range f.Untested {
			all = append(all, probe{untested, detail})
		}
		for _, detail := range f.Unsupported {
			all = append(all, probe{unsupported, detail})
		}
		if len(all) == 0 {
			continue
		}
		counts := map[outcome]int{}
		notes := map[string]bool{}
		for _, p := range all {
			counts[p.outcome]++
			if p.outcome != runs {
				notes[p.detail] = true
			}
		}
		st := status(counts)
		totals[st]++
		note := slices.DeleteFunc([]string{f.Note}, func(s string) bool { return s == "" })
		note = append(note, sortedNames(notes)...)
		fmt.Fprintf(&body, "| [`%s`](%s/%s) | %s | %s | %s | %s |\n", f.Name, functionDocs, mainDoc(f),
			categories(f), st, f.Implementation, escape(strings.Join(note, "; ")))
	}
	fmt.Fprint(w, functionsPreamble)
	writeTotals(w, "Functions", totals)
	fmt.Fprint(w, "\n| Function | Category | Status | Implementation | Notes |\n| --- | --- | --- | --- | --- |\n")
	_, err = body.WriteTo(w)
	return err
}
