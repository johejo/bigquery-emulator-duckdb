package main

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"runtime"
	"strings"

	"github.com/johejo/bigquery-emulator-duckdb/internal/emulatorprocess"
	"github.com/johejo/bigquery-emulator-duckdb/internal/functioncoverage"
	"github.com/johejo/bigquery-emulator-duckdb/internal/probereport"
)

//go:embed function_types_preamble.md
var functionTypesPreamble string

type typedCall struct {
	function  functioncoverage.Function
	signature functioncoverage.Signature
	sql       string
	probe     probereport.Probe
}

func typeCalls(c functioncoverage.Catalog) []typedCall {
	var calls []typedCall
	for _, f := range c.Functions {
		for _, s := range f.Signatures {
			sql, reason := functioncoverage.Query(f, s)
			call := typedCall{function: f, signature: s, sql: sql}
			if reason != "" {
				call.probe = probereport.Probe{Outcome: probereport.Untested, Detail: reason}
			}
			calls = append(calls, call)
		}
	}
	return calls
}

// runTypedCall also checks the REST result schema against the documented return type. Values,
// NULL propagation and boundary behavior belong to end-to-end tests, not this probe.
func runTypedCall(url, sql, want string) (probereport.Probe, error) {
	body, err := json.Marshal(map[string]any{"query": sql, "useLegacySql": false})
	if err != nil {
		return probereport.Probe{}, err
	}
	r, err := send(url, "POST", "/bigquery/v2/projects/test/queries", string(body))
	if err != nil {
		return probereport.Probe{}, err
	}
	return typedResponse(r, sql, want)
}

func typedResponse(r response, sql, want string) (probereport.Probe, error) {
	var body map[string]json.RawMessage
	if err := json.Unmarshal(r.body, &body); err != nil {
		return probereport.Probe{}, err
	}
	if message, failed := responseError(body); failed {
		return classify(sql, message), nil
	}
	if r.status != 200 {
		return probereport.Probe{}, fmt.Errorf("jobs.query: HTTP %d: %s", r.status, r.body)
	}
	var schema struct {
		Fields []functioncoverage.Field `json:"fields"`
	}
	if err := json.Unmarshal(body["schema"], &schema); err != nil {
		return probereport.Probe{}, fmt.Errorf("query schema: %w", err)
	}
	if len(schema.Fields) != 1 {
		return probereport.Probe{}, fmt.Errorf("query returned %d fields, want one", len(schema.Fields))
	}
	got, err := schema.Fields[0].SQLType()
	if err != nil {
		return probereport.Probe{}, err
	}
	if got != want {
		return probereport.Probe{Outcome: probereport.Broken, Detail: "return type " + got + ", documented " + want}, nil
	}
	return probereport.Probe{Outcome: probereport.Runs}, nil
}

func probeFunctionTypes(w io.Writer, binary, catalogPath, functionsPath string) error {
	catalog, err := functioncoverage.Read(catalogPath)
	if err != nil {
		return err
	}
	lines, err := readLines(functionsPath)
	if err != nil {
		return err
	}
	known := map[string]bool{}
	for _, l := range lines {
		known[l.directive] = true
	}
	for _, f := range catalog.Functions {
		if !known[f.Name] {
			return fmt.Errorf("%s: function %s is not in %s", catalogPath, f.Name, functionsPath)
		}
		if _, err := os.Stat(f.Source); err != nil {
			return err
		}
	}
	calls := typeCalls(catalog)
	if len(calls) > 0 {
		// Pure scalar/aggregate calls need no graph or external database fixtures.
		workers := min(runtime.NumCPU(), 8, len(calls))
		emulators := make(chan *emulatorprocess.Process, workers)
		defer func() {
			close(emulators)
			for e := range emulators {
				e.Stop()
			}
		}()
		if err := parallel(workers, func(int) error {
			e, err := startEmulator(binary)
			if err == nil {
				emulators <- e
			}
			return err
		}); err != nil {
			return err
		}
		if err := parallel(len(calls), func(i int) error {
			if calls[i].sql == "" {
				return nil
			}
			e := <-emulators
			defer func() { emulators <- e }()
			var err error
			calls[i].probe, err = runTypedCall(e.URL, calls[i].sql, calls[i].signature.Result)
			return err
		}); err != nil {
			return err
		}
	}
	writeFunctionTypes(w, known, catalog, calls)
	return nil
}

func writeFunctionTypes(w io.Writer, known map[string]bool, catalog functioncoverage.Catalog, calls []typedCall) {
	byName := map[string][]typedCall{}
	for _, call := range calls {
		byName[call.function.Name] = append(byName[call.function.Name], call)
	}
	fmt.Fprint(w, functionTypesPreamble)
	fmt.Fprintf(w, "\nType signatures are enumerated for **%d of %d functions**. The remaining **%d** are not enumerated; their function-level probes are listed in [functions.md](functions.md).\n", len(catalog.Functions), len(known), len(known)-len(catalog.Functions))
	fmt.Fprint(w, "\n## Coverage\n\n| Function | Enumerated forms | Status |\n| --- | --- | --- |\n")
	for _, name := range probereport.SortedNames(known) {
		entries := byName[name]
		if len(entries) == 0 {
			fmt.Fprintf(w, "| `%s` | 0 | Not enumerated |\n", name)
			continue
		}
		counts := map[probereport.Outcome]int{}
		for _, call := range entries {
			counts[call.probe.Outcome]++
		}
		status := probereport.Status(counts)
		// The shared report's status ignores untested calls when some run. Here incomplete type
		// coverage must never become Supported merely because all the calls we could build ran.
		if counts[probereport.Untested] > 0 && status != "Broken" {
			status = "Untested"
		}
		fmt.Fprintf(w, "| [%s](#%s) | %d | %s |\n", name, strings.ToLower(strings.ReplaceAll(name, ".", "")), len(entries), status)
	}
	for _, f := range catalog.Functions {
		fmt.Fprintf(w, "\n## %s\n\nSource: [%s](../%s).\n", f.Name, f.Name, f.Source)
		if f.Note != "" {
			fmt.Fprintf(w, "\n%s\n", f.Note)
		}
		fmt.Fprint(w, "\n| Input types | Return type | Status | Notes |\n| --- | --- | --- | --- |\n")
		for _, call := range byName[f.Name] {
			status := probereport.Status(map[probereport.Outcome]int{call.probe.Outcome: 1})
			fmt.Fprintf(w, "| `%s` | `%s` | %s | %s |\n", strings.Join(call.signature.Arguments, ", "), call.signature.Result, status, probereport.Escape(call.probe.Detail))
		}
	}
}
