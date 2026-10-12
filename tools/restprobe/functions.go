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

// categoryTitles names the pages whose titles words do not spell.
var categoryTitles = map[string]string{
	"aggregate-dp-functions":   "Differentially private aggregate functions",
	"objectref_functions":      "ObjectRef functions",
	"table-functions-built-in": "Built-in table functions",
	"vectorindex_functions":    "Vector index functions",
}

// acronyms are the words of page names that titles spell in capitals.
var acronyms = map[string]bool{"aead": true, "dlp": true, "hll": true, "json": true, "kll": true,
	"sql": true}

// category titles the page that documents the function, such as "String functions" for
// string_functions.
func category(f *function) string {
	page, _, _ := strings.Cut(mainDoc(f), "#")
	if title, ok := categoryTitles[page]; ok {
		return title
	}
	words := strings.FieldsFunc(page, func(r rune) bool { return r == '_' || r == '-' })
	for i, word := range words {
		if acronyms[word] {
			words[i] = strings.ToUpper(word)
		}
	}
	title := strings.Join(words, " ")
	return strings.ToUpper(title[:1]) + title[1:]
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

	rows := map[string][]string{}
	counts := map[string]map[string]int{}
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
		outcomes := map[outcome]int{}
		notes := map[string]bool{}
		for _, p := range all {
			outcomes[p.outcome]++
			if p.outcome != runs {
				notes[p.detail] = true
			}
		}
		st := status(outcomes)
		c := category(f)
		if counts[c] == nil {
			counts[c] = map[string]int{}
		}
		counts[c][st]++
		note := slices.DeleteFunc([]string{f.Note}, func(s string) bool { return s == "" })
		note = append(note, sortedNames(notes)...)
		rows[c] = append(rows[c], fmt.Sprintf("| [`%s`](%s/%s) | %s | %s | %s |\n", f.Name,
			functionDocs, mainDoc(f), st, f.Implementation, escape(strings.Join(note, "; "))))
	}
	// Sort categories as words, so that "Date functions" precedes "DLP functions".
	categories := sortedNames(rows)
	slices.SortFunc(categories, func(a, b string) int {
		return strings.Compare(strings.ToLower(a), strings.ToLower(b))
	})
	var tallies []tally
	for _, c := range categories {
		tallies = append(tallies, tally{c, counts[c]})
	}
	fmt.Fprint(w, functionsPreamble)
	writeSummary(w, "Category", tallies)
	for _, c := range categories {
		fmt.Fprintf(w, "\n## %s\n\n| Function | Status | Implementation | Notes |\n| --- | --- | --- | --- |\n", c)
		for _, row := range rows[c] {
			fmt.Fprint(w, row)
		}
	}
	return nil
}
