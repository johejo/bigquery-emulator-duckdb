package main

import (
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/johejo/bigquery-emulator-duckdb/internal/functioncoverage"
)

func TestProseTypeConstraints(t *testing.T) {
	entries, err := proseDomains(filepath.Join("..", "..", "third_party", "bigquery_docs"))
	if err != nil {
		t.Fatal(err)
	}
	functions := map[string]functioncoverage.Function{}
	for _, f := range entries {
		functions[f.Name] = f
	}
	has := func(name string, args ...string) bool {
		return slices.ContainsFunc(functions[name].Signatures, func(s functioncoverage.Signature) bool { return slices.Equal(s.Arguments, args) })
	}
	for _, name := range []string{"FARM_FINGERPRINT", "MD5", "SHA1", "SHA256", "SHA512", "BYTE_LENGTH", "LOWER"} {
		if !has(name, "STRING") || !has(name, "BYTES") {
			t.Errorf("%s lacks string/bytes coverage", name)
		}
	}
	for _, name := range []string{"TRIM", "LTRIM", "RTRIM", "SPLIT"} {
		if !has(name, "STRING") || has(name, "BYTES") || !has(name, "BYTES", "BYTES") {
			t.Errorf("%s lost required BYTES argument constraint", name)
		}
	}
	for _, s := range functions["SPLIT"].Signatures {
		if len(s.Arguments) == 2 && (!slices.Contains(s.LiteralArguments, 1) || s.Samples[1] == "") {
			t.Error("SPLIT delimiter must be literal")
		}
	}
	for _, s := range functions["ARRAY_AGG"].Signatures {
		if strings.HasPrefix(s.Arguments[0], "ARRAY<") {
			t.Error("ARRAY_AGG cannot take ARRAY")
		}
	}
	for _, name := range []string{"MIN", "MAX"} {
		for _, s := range functions[name].Signatures {
			if s.Arguments[0] == "GEOGRAPHY" || s.Arguments[0] == "JSON" || strings.HasPrefix(s.Arguments[0], "ARRAY<") || strings.HasPrefix(s.Arguments[0], "STRUCT<") {
				t.Errorf("non-orderable argument to %s", name)
			}
		}
	}
	for _, tpe := range []string{"GEOGRAPHY", "JSON", "INTERVAL", "RANGE<DATE>", "ARRAY<STRUCT<items ARRAY<BIGNUMERIC>, span RANGE<DATE>>>"} {
		if !has("ANY_VALUE", tpe) {
			t.Errorf("ANY_VALUE lacks %s", tpe)
		}
	}
	for _, name := range []string{"CORR", "COVAR_POP", "COVAR_SAMP"} {
		if len(functions[name].Signatures) != 16 {
			t.Errorf("%s lacks mixed numeric pairs", name)
		}
	}
	for _, name := range []string{"CONCAT", "GENERATE_ARRAY", "COALESCE", "IFNULL", "IF"} {
		if functions[name].Note == "" {
			t.Errorf("%s needs its incomplete expansion documented", name)
		}
	}
}

func TestProseDomainsRequireSavedSources(t *testing.T) {
	if _, err := proseDomains(t.TempDir()); err == nil {
		t.Fatal("accepted missing source snapshots")
	}
}
