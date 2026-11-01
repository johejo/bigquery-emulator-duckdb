package main

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"os"
	"runtime"

	"github.com/johejo/bigquery-emulator-duckdb/internal/functioncoverage"
	"github.com/johejo/bigquery-emulator-duckdb/internal/probereport"
	"github.com/johejo/bigquery-emulator-duckdb/internal/serverprocess"
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

func probeFunctionTypes(binary, catalogPath, functionsPath, outputDir string, check bool) error {
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
		emulators := make(chan *serverprocess.Emulator, workers)
		defer func() {
			close(emulators)
			for e := range emulators {
				e.Stop()
			}
		}()
		if err := parallel(workers, func(int) error {
			// SESSION_USER needs an identity to run.
			e, err := startEmulator(binary, "--session-user", "jdoe@example.com")
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
	return syncFunctionTypeReports(outputDir, functionTypeReports(known, catalog, calls), check)
}
