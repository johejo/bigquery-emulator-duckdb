package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/johejo/bigquery-emulator-duckdb/internal/functioncoverage"
	"github.com/johejo/bigquery-emulator-duckdb/internal/probereport"
)

func sampleTypeReports() map[string][]byte {
	catalog := functioncoverage.Catalog{Functions: []functioncoverage.Function{
		{Name: "ABS", Source: "third_party/bigquery_docs/reference/standard-sql/mathematical_functions.md", Note: "A coverage note.", Signatures: []functioncoverage.Signature{{Arguments: []string{"INT64"}, Result: "INT64"}}},
		{Name: "MD5", Source: "third_party/bigquery_docs/reference/standard-sql/hash_functions.md", Signatures: []functioncoverage.Signature{{Arguments: []string{"STRING"}, Result: "BYTES"}}},
	}}
	calls := typeCalls(catalog)
	for i := range calls {
		calls[i].probe.Outcome = probereport.Runs
	}
	return functionTypeReports(map[string]bool{"ABS": true, "MD5": true, "OTHER": true}, catalog, calls)
}

func TestFunctionTypeDomainPages(t *testing.T) {
	reports := sampleTypeReports()
	if len(reports) != 3 {
		t.Fatalf("got %d pages, want index and two domains", len(reports))
	}
	index := string(reports["function-types.md"])
	for _, fragment := range []string{"[ABS](function-types/mathematical.md#abs)", "[MD5](function-types/hash.md#md5)", "| `OTHER` | 0 | Not enumerated |"} {
		if !strings.Contains(index, fragment) {
			t.Errorf("index lacks %s", fragment)
		}
	}
	if strings.Contains(index, "## ABS") || strings.Contains(index, "| Input types |") {
		t.Error("index still includes signature details")
	}
	for _, tt := range []struct{ domain, name, other string }{{"mathematical", "ABS", "MD5"}, {"hash", "MD5", "ABS"}} {
		body := string(reports["function-types/"+tt.domain+".md"])
		for _, fragment := range []string{functionTypesGenerated, "[All functions and coverage notes](../function-types.md)", "[" + tt.name + "](#" + strings.ToLower(tt.name) + ")", "## " + tt.name, "(../../third_party/bigquery_docs/reference/standard-sql/", "| Input types |"} {
			if !strings.Contains(body, fragment) {
				t.Errorf("%s lacks %s", tt.domain, fragment)
			}
		}
		if strings.Contains(body, "## "+tt.other) {
			t.Errorf("%s leaks another domain's details", tt.domain)
		}
	}
	if !strings.Contains(string(reports["function-types/mathematical.md"]), "A coverage note.") {
		t.Error("lost function-specific scope note")
	}
	for source, want := range map[string]string{"statistical_aggregate_functions.md": "statistical-aggregate", "conditional_expressions.md": "conditional", "range-functions.md": "range"} {
		if got := functionTypeDomain(source); got != want {
			t.Errorf("domain %s: got %s, want %s", source, got, want)
		}
	}
}

func TestSyncFunctionTypeReports(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "docs")
	reports := sampleTypeReports()
	if err := syncFunctionTypeReports(dir, reports, true); err == nil {
		t.Fatal("check accepted missing files")
	}
	if _, err := os.Stat(dir); !os.IsNotExist(err) {
		t.Fatal("check created output directory")
	}
	if err := syncFunctionTypeReports(dir, reports, false); err != nil {
		t.Fatal(err)
	}
	if err := syncFunctionTypeReports(dir, reports, true); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"function-types.md", "function-types/hash.md"} {
		path := filepath.Join(dir, name)
		if err := os.WriteFile(path, []byte("modified"), 0600); err != nil {
			t.Fatal(err)
		}
		if err := syncFunctionTypeReports(dir, reports, true); err == nil || !strings.Contains(err.Error(), "stale") {
			t.Fatalf("check missed changed %s: %v", name, err)
		}
		got, err := os.ReadFile(path)
		if err != nil || string(got) != "modified" {
			t.Fatal("check rewrote stale file")
		}
		if err := syncFunctionTypeReports(dir, reports, false); err != nil {
			t.Fatal(err)
		}
	}
	obsolete := filepath.Join(dir, "function-types", "obsolete.md")
	unrelated := filepath.Join(dir, "functions.md")
	for _, path := range []string{obsolete, unrelated} {
		if err := os.WriteFile(path, []byte("preserve unless owned by generator"), 0600); err != nil {
			t.Fatal(err)
		}
	}
	if err := syncFunctionTypeReports(dir, reports, true); err == nil || !strings.Contains(err.Error(), "obsolete") {
		t.Fatalf("check missed obsolete page: %v", err)
	}
	if err := syncFunctionTypeReports(dir, reports, false); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(obsolete); !os.IsNotExist(err) {
		t.Fatal("obsolete generated page was not removed")
	}
	if _, err := os.Stat(unrelated); err != nil {
		t.Fatal("removed unrelated documentation")
	}
	missing := filepath.Join(dir, "function-types", "hash.md")
	if err := os.Remove(missing); err != nil {
		t.Fatal(err)
	}
	if err := syncFunctionTypeReports(dir, reports, true); err == nil {
		t.Fatal("check missed missing detail page")
	}
	if _, err := os.Stat(missing); !os.IsNotExist(err) {
		t.Fatal("check recreated missing page")
	}
}
