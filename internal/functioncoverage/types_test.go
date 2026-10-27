package functioncoverage

import (
	"encoding/json"
	"reflect"
	"strings"
	"testing"
)

func TestNestedSchemaTypes(t *testing.T) {
	for _, tt := range []struct{ body, want string }{
		{`{"type":"RECORD","fields":[{"name":"x","type":"INTEGER"},{"name":"label","type":"STRING"}]}`, "STRUCT<x INT64, label STRING>"},
		{`{"type":"RECORD","mode":"REPEATED","fields":[{"name":"items","type":"BIGNUMERIC","mode":"REPEATED"},{"name":"span","type":"RANGE","rangeElementType":{"type":"DATE"}}]}`, "ARRAY<STRUCT<items ARRAY<BIGNUMERIC>, span RANGE<DATE>>>"},
		{`{"type":"RECORD","fields":[{"name":"nested","type":"RECORD","fields":[{"name":"flag","type":"BOOLEAN"}]}]}`, "STRUCT<nested STRUCT<flag BOOL>>"},
		{`{"type":"RANGE","mode":"REPEATED","rangeElementType":{"type":"TIMESTAMP"}}`, "ARRAY<RANGE<TIMESTAMP>>"},
	} {
		var f Field
		if err := json.Unmarshal([]byte(tt.body), &f); err != nil {
			t.Fatal(err)
		}
		got, err := f.SQLType()
		if err != nil || got != tt.want {
			t.Errorf("schema %s: got %q, %v; want %q", tt.body, got, err, tt.want)
		}
	}
	for _, body := range []string{`{}`, `{"type":"RANGE"}`, `{"type":"RANGE","rangeElementType":{"type":"INT64"}}`, `{"type":"RECORD","fields":[{"type":"INTEGER"}]}`} {
		var f Field
		if err := json.Unmarshal([]byte(body), &f); err != nil {
			t.Fatal(err)
		}
		if _, err := f.SQLType(); err == nil {
			t.Errorf("accepted invalid schema %s", body)
		}
	}
}

func TestStructuralSamples(t *testing.T) {
	for _, typ := range []string{
		"JSON", "GEOGRAPHY", "ARRAY<DATE>", "ARRAY<JSON>", "RANGE<DATE>", "RANGE<DATETIME>", "RANGE<TIMESTAMP>",
		"STRUCT<x INT64, label STRING>", "STRUCT<nested STRUCT<flag BOOL>>",
		"ARRAY<STRUCT<items ARRAY<BIGNUMERIC>, span RANGE<DATE>>>",
	} {
		sample, ok := Sample(typ)
		if !ok || sample == "" {
			t.Errorf("no sample for %s", typ)
		}
	}
	for _, typ := range []string{"ARRAY<ARRAY<INT64>>", "RANGE<INT64>", "ARRAY<NO_SAMPLE>", "STRUCT<x NO_SAMPLE>", "STRUCT<INT64>", "STRUCT<x ARRAY<INT64>"} {
		if sample, ok := Sample(typ); ok {
			t.Errorf("accepted invalid/unsampled type %s: %s", typ, sample)
		}
	}
	sample, _ := Sample("STRUCT<items ARRAY<BIGNUMERIC>, span RANGE<DATE>>")
	for _, fragment := range []string{"ARRAY<BIGNUMERIC>[CAST(2 AS BIGNUMERIC)]", "RANGE<DATE> '[2024-01-01, 2024-02-01)'"} {
		if !strings.Contains(sample, fragment) {
			t.Errorf("sample %s lacks %s", sample, fragment)
		}
	}
}

func TestSplitTypes(t *testing.T) {
	got, err := SplitTypes("items ARRAY<STRUCT<x INT64, y STRING>>, flag BOOL")
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(got, []string{"items ARRAY<STRUCT<x INT64, y STRING>>", "flag BOOL"}) {
		t.Fatalf("got %v", got)
	}
	for _, text := range []string{"ARRAY<INT64", "INT64>", "INT64,", ",INT64"} {
		if _, err := SplitTypes(text); err == nil {
			t.Errorf("accepted %s", text)
		}
	}
}
