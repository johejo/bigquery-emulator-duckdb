package main

import (
	"path/filepath"
	"reflect"
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
	for _, name := range []string{"ATAN2", "IEEE_DIVIDE"} {
		if len(functions[name].Signatures) != 16 {
			t.Errorf("%s lacks mixed numeric pairs", name)
		}
	}
	for _, name := range []string{"ACOS", "ASIN", "ATANH"} {
		for _, s := range functions[name].Signatures {
			want := "1"
			if name == "ATANH" {
				want = "0"
			}
			if s.Result != "FLOAT64" || len(s.Samples) != 1 || s.Samples[0] != want {
				t.Errorf("%s needs a valid inverse-function sample and FLOAT64 result: %+v", name, s)
			}
		}
	}
	for _, name := range []string{"CHAR_LENGTH", "CHARACTER_LENGTH", "UNICODE", "FROM_BASE32", "FROM_BASE64", "FROM_HEX", "INITCAP", "NORMALIZE", "NORMALIZE_AND_CASEFOLD", "SOUNDEX"} {
		if !has(name, "STRING") || has(name, "BYTES") {
			t.Errorf("%s must cover STRING, not BYTES", name)
		}
	}
	for _, name := range []string{"TO_BASE32", "TO_BASE64", "TO_HEX", "SAFE_CONVERT_BYTES_TO_STRING"} {
		if !has(name, "BYTES") || has(name, "STRING") {
			t.Errorf("%s must cover BYTES, not STRING", name)
		}
	}
	if !has("RAND") || functions["RAND"].Signatures[0].Result != "FLOAT64" {
		t.Error("RAND needs a zero-argument FLOAT64 form")
	}
	for _, name := range []string{"NET.IP_FROM_STRING", "NET.SAFE_IP_FROM_STRING", "NET.IP_NET_MASK", "NET.IP_TO_STRING", "NET.IP_TRUNC", "NET.IPV4_TO_INT64", "FROM_BASE32", "FROM_BASE64", "FROM_HEX"} {
		for _, s := range functions[name].Signatures {
			if len(s.Samples) != len(s.Arguments) || s.Samples[0] == "" {
				t.Errorf("%s needs a valid function-specific sample", name)
			}
			if _, reason := functioncoverage.Query(functions[name], s); reason != "" {
				t.Errorf("%s: %s", name, reason)
			}
		}
	}
	for _, name := range []string{"CONCAT", "GENERATE_ARRAY", "COALESCE", "IFNULL", "IF", "GREATEST", "LEAST", "NORMALIZE", "NORMALIZE_AND_CASEFOLD", "EDIT_DISTANCE"} {
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

func TestExpandDomainsSyntax(t *testing.T) {
	root := filepath.Join("..", "..", "third_party", "bigquery_docs")
	got, err := expandDomains(root, `
types @a = STRING | STRUCT<x INT64, y BYTES>
types @b = @a | ARRAY<@a> | -STRING | -ARRAY<STRUCT<x INT64, y BYTES>>
page string_functions
for $T in @b
  LPAD($T, INT64[, literal $T = 'x, [y]']) -> $T
end
note LPAD n
`)
	if err != nil {
		t.Fatal(err)
	}
	want := []functioncoverage.Signature{
		{Arguments: []string{"STRUCT<x INT64, y BYTES>", "INT64"}, Result: "STRUCT<x INT64, y BYTES>"},
		{Arguments: []string{"STRUCT<x INT64, y BYTES>", "INT64", "STRUCT<x INT64, y BYTES>"}, Result: "STRUCT<x INT64, y BYTES>", Samples: []string{"", "", "'x, [y]'"}, LiteralArguments: []int{2}},
		{Arguments: []string{"ARRAY<STRING>", "INT64"}, Result: "ARRAY<STRING>"},
		{Arguments: []string{"ARRAY<STRING>", "INT64", "ARRAY<STRING>"}, Result: "ARRAY<STRING>", Samples: []string{"", "", "'x, [y]'"}, LiteralArguments: []int{2}},
	}
	if len(got) != 1 || got[0].Note != "n" || !reflect.DeepEqual(got[0].Signatures, want) {
		t.Fatalf("expanded: %#v", got)
	}
	for _, text := range []string{
		"page string_functions\nLPAD($T) -> STRING",
		"page string_functions\nfor $T in STRING\nLPAD($T) -> $T",
		"page string_functions\nend",
		"types @a = STRING | -BYTES",
		"page string_functions\nfor $T in @missing\nend",
		"page string_functions\nLPAD(STRING[, INT64], INT64) -> STRING",
		"page string_functions\nLPAD(STRING, , INT64) -> STRING",
		"page string_functions\nNOT_DOCUMENTED(STRING) -> STRING",
		"page string_functions\nLPAD(STRING) -> STRING\npage hash_functions\nLPAD(BYTES) -> BYTES",
		"note LPAD n",
	} {
		if _, err := expandDomains(root, text); err == nil {
			t.Errorf("accepted %q", text)
		}
	}
}
