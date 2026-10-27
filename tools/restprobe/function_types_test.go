package main

import (
	"strings"
	"testing"

	"github.com/johejo/bigquery-emulator-duckdb/internal/functioncoverage"
	"github.com/johejo/bigquery-emulator-duckdb/internal/probereport"
)

func TestTypedResponse(t *testing.T) {
	for _, tt := range []struct {
		name, body, want string
		outcome          probereport.Outcome
	}{
		{"integer alias", `{"schema":{"fields":[{"type":"INTEGER"}]}}`, "INT64", probereport.Runs},
		{"float alias", `{"schema":{"fields":[{"type":"FLOAT"}]}}`, "FLOAT64", probereport.Runs},
		{"boolean alias", `{"schema":{"fields":[{"type":"BOOLEAN"}]}}`, "BOOL", probereport.Runs},
		{"wrong type", `{"schema":{"fields":[{"type":"NUMERIC"}]}}`, "BIGNUMERIC", probereport.Broken},
		{"nested record", `{"schema":{"fields":[{"type":"RECORD","fields":[{"name":"x","type":"INTEGER"}]}]}}`, "STRUCT<x INT64>", probereport.Runs},
		{"wrong nested type", `{"schema":{"fields":[{"type":"RECORD","fields":[{"name":"x","type":"NUMERIC"}]}]}}`, "STRUCT<x BIGNUMERIC>", probereport.Broken},
		{"range", `{"schema":{"fields":[{"type":"RANGE","rangeElementType":{"type":"DATE"}}]}}`, "RANGE<DATE>", probereport.Runs},
		{"wrong range element", `{"schema":{"fields":[{"type":"RANGE","rangeElementType":{"type":"DATETIME"}}]}}`, "RANGE<DATE>", probereport.Broken},
		{"wrong cardinality", `{"schema":{"fields":[{"type":"INTEGER","mode":"REPEATED"}]}}`, "INT64", probereport.Broken},
		{"unsupported", `{"error":{"message":"The emulator does not support F"}}`, "INT64", probereport.Unsupported},
		{"analyzer rejection", `{"error":{"message":"INVALID_ARGUMENT: No matching signature"}}`, "INT64", probereport.Untested},
		{"execution error", `{"error":{"message":"Binder Error: no function"}}`, "INT64", probereport.Broken},
	} {
		t.Run(tt.name, func(t *testing.T) {
			got, err := typedResponse(response{status: 200, body: []byte(tt.body)}, "SELECT F()", tt.want)
			if err != nil {
				t.Fatal(err)
			}
			if got.Outcome != tt.outcome {
				t.Fatalf("got %+v; want %v", got, tt.outcome)
			}
		})
	}
	for _, body := range []string{`not JSON`, `{}`, `{"schema":{"fields":[]}}`} {
		if _, err := typedResponse(response{status: 200, body: []byte(body)}, "SELECT F()", "INT64"); err == nil {
			t.Errorf("accepted malformed response %s", body)
		}
	}
}

func TestTypeCoverageKeepsUntestedAndUnenumerated(t *testing.T) {
	catalog := functioncoverage.Catalog{Functions: []functioncoverage.Function{{Name: "F", Source: "third_party/bigquery_docs/reference/standard-sql/mathematical_functions.md", Signatures: []functioncoverage.Signature{
		{Arguments: []string{"INT64"}, Result: "INT64"}, {Arguments: []string{"NO_SAMPLE"}, Result: "INT64"},
	}}}}
	calls := typeCalls(catalog)
	if len(calls) != 2 || calls[1].probe.Outcome != probereport.Untested {
		t.Fatalf("missing untested form: %+v", calls)
	}
	reports := functionTypeReports(map[string]bool{"F": true, "G": true}, catalog, calls)
	for _, fragment := range []string{"**1 of 2 functions**", "| [F](function-types/mathematical.md#f) | 2 | Untested |", "| `G` | 0 | Not enumerated |"} {
		if !strings.Contains(string(reports["function-types.md"]), fragment) {
			t.Errorf("index lacks %q", fragment)
		}
	}
	if !strings.Contains(string(reports["function-types/mathematical.md"]), "no sample for NO_SAMPLE") {
		t.Error("detail page lost untested form")
	}
}
