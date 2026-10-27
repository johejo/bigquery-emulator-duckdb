package main

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/johejo/bigquery-emulator-duckdb/internal/functioncoverage"
)

func TestExtract(t *testing.T) {
	text := "## `F`\n\n| INPUT | `INT64` | `FLOAT64` |\n| --- | --- | --- |\n| OUTPUT | `INT64` | `FLOAT64` |\n"
	got, err := extract("mathematical_functions", text)
	if err != nil {
		t.Fatal(err)
	}
	want := []functioncoverage.Signature{{Arguments: []string{"INT64"}, Result: "INT64"}, {Arguments: []string{"FLOAT64"}, Result: "FLOAT64"}}
	if len(got) != 1 || got[0].Name != "F" || !reflect.DeepEqual(got[0].Signatures, want) {
		t.Fatalf("extracted: %#v", got)
	}
}

func TestMatrix(t *testing.T) {
	rows := [][]string{{"INPUT", "INT64", "FLOAT64"}, {"---", "---", "---"}, {"INT64", "INT64", "FLOAT64"}, {"FLOAT64", "FLOAT64", "FLOAT64"}}
	got, err := tableSignatures(rows)
	if err != nil {
		t.Fatal(err)
	}
	want := []functioncoverage.Signature{
		{Arguments: []string{"INT64", "INT64"}, Result: "INT64"},
		{Arguments: []string{"INT64", "FLOAT64"}, Result: "FLOAT64"},
		{Arguments: []string{"FLOAT64", "INT64"}, Result: "FLOAT64"},
		{Arguments: []string{"FLOAT64", "FLOAT64"}, Result: "FLOAT64"},
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("matrix: %#v", got)
	}
}

func TestRejectChangedTableShape(t *testing.T) {
	for _, rows := range [][][]string{
		{{"INPUT", "INT64"}},
		{{"INPUT", "INT64"}, {"---", "---"}, {"OUTPUT"}},
		{{"INPUT", "INT64"}, {"---", "---"}, {"OUTPUT", "NEW_TYPE"}},
		{{"INPUT", "INT64", "FLOAT64"}, {"---", "---", "---"}, {"INT64", "INT64", "FLOAT64"}},
		{{"INPUT", "INT64"}, {"---", "---"}, {"FLOAT64", "FLOAT64"}},
	} {
		if _, err := tableSignatures(rows); err == nil {
			t.Fatalf("accepted changed table: %v", rows)
		}
	}
}

func TestCheckedInCatalog(t *testing.T) {
	root := filepath.Join("..", "..")
	got, err := generate(filepath.Join(root, "third_party", "bigquery_docs"))
	if err != nil {
		t.Fatal(err)
	}
	want, err := functioncoverage.Read(filepath.Join(root, "tools", "function_types.json"))
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatal("catalog differs from saved documentation; run just function-types-update")
	}
	// Every source is an actual local snapshot, not an unacquired citation.
	for _, f := range got.Functions {
		if _, err := os.Stat(filepath.Join(root, f.Source)); err != nil {
			t.Fatal(err)
		}
	}
}

func TestGenerationKeepsAllMixedPairsAndOptionalForms(t *testing.T) {
	c, err := generate(filepath.Join("..", "..", "third_party", "bigquery_docs"))
	if err != nil {
		t.Fatal(err)
	}
	byName := map[string]functioncoverage.Function{}
	for _, f := range c.Functions {
		byName[f.Name] = f
	}
	for _, name := range []string{"POW", "POWER", "SAFE_ADD", "SAFE_DIVIDE"} {
		if len(byName[name].Signatures) != 16 {
			t.Errorf("%s: missing mixed-type pairs", name)
		}
	}
	if !reflect.DeepEqual(byName["CEIL"].Signatures, byName["CEILING"].Signatures) {
		t.Error("alias lost signatures")
	}
	for _, f := range c.Functions {
		for _, s := range f.Signatures {
			sql, reason := functioncoverage.Query(f, s)
			if reason != "" || !strings.HasPrefix(sql, "SELECT ") {
				t.Errorf("%s %v: %s", f.Name, s, reason)
			}
		}
	}
	if len(byName["ROUND"].Signatures) != 10 || len(byName["TRUNC"].Signatures) != 8 || len(byName["LOG"].Signatures) != 20 {
		t.Error("missing optional forms")
	}
}
