package main

import (
	"bytes"
	"strings"
	"testing"

	"github.com/johejo/bigquery-emulator-duckdb/internal/ddloptions"
	"github.com/johejo/bigquery-emulator-duckdb/internal/probereport"
)

func TestOptionsSharedHeading(t *testing.T) {
	catalog := ddloptions.Catalog{
		Source: "reference.md",
		Groups: map[string][]ddloptions.Option{
			"uri":     {{Name: "uris", Type: "STRING", Values: []string{"'https://example.com'"}}},
			"options": {{Name: "bigtable_options", Type: "STRING", Values: []string{"'{}'"}}},
		},
		Contexts: []ddloptions.Context{
			{Name: "Bigtable URI", Heading: "Bigtable", Group: "uri", Template: "CREATE T OPTIONS ({option})"},
			{Name: "Bigtable", Group: "options", Template: "CREATE T OPTIONS (uris = 'https://example.com', {option})"},
		},
	}
	if err := catalog.Validate(); err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	if err := writeOptions(&output, catalog.Cases(), [][]probereport.Probe{
		{{Outcome: probereport.Runs}},
		{{Outcome: probereport.Unsupported}},
	}); err != nil {
		t.Fatal(err)
	}
	report := output.String()
	if strings.Count(report, "\n## Bigtable\n") != 1 || strings.Contains(report, "Bigtable URI") {
		t.Fatalf("expected a single shared heading:\n%s", report)
	}
	for _, row := range []string{
		"| [Bigtable](#bigtable) | 1 | 1 |",
		"| [`uris`](reference.md#uri) | `STRING` | Supported |",
		"| [`bigtable_options`](reference.md#options) | `STRING` | Unsupported |",
	} {
		if !strings.Contains(report, row) {
			t.Errorf("missing %q in report:\n%s", row, report)
		}
	}
}
