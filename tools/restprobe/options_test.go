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
			"uri": {{Name: "uris", Type: "STRING", Values: []string{"'https://example.com'"}}},
			"options": {{
				Name: "bigtable_options", Type: "STRING",
				Values:      []string{"'{}'", "'a'", "'b'"},
				ValueLabels: map[string]string{"'{}'": "accepted form", "'a'": "rejected form"},
			}},
			"blocked": {{Name: "description", Type: "STRING", Values: []string{"'probe'"}}},
		},
		Contexts: []ddloptions.Context{
			{Name: "Bigtable URI", Heading: "Bigtable", Group: "uri", Template: "CREATE T OPTIONS ({option})"},
			{Name: "Bigtable", Group: "options", Template: "CREATE T OPTIONS (uris = 'https://example.com', {option})"},
			{Name: "ALTER T", Group: "blocked", Template: "ALTER T SET OPTIONS ({option})"},
		},
	}
	if err := catalog.Validate(); err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	runs := optionProbe{Probe: probereport.Probe{Outcome: probereport.Runs}}
	unsupported := optionProbe{Probe: probereport.Probe{Outcome: probereport.Unsupported}}
	if err := writeOptions(&output, catalog.Cases(), [][]optionProbe{
		{runs},
		{runs, unsupported, unsupported},
		{{Probe: unsupported.Probe, blockedBy: "CREATE T"}},
	}); err != nil {
		t.Fatal(err)
	}
	report := output.String()
	if strings.Count(report, "\n## Bigtable\n") != 1 || strings.Contains(report, "Bigtable URI") {
		t.Fatalf("expected a single shared heading:\n%s", report)
	}
	if strings.Contains(report, "accepted form") || strings.Contains(report, "bigtable_options = 'a'") {
		t.Fatalf("expected only rejected forms, using the label when present:\n%s", report)
	}
	for _, row := range []string{
		"| [Bigtable](#bigtable) | 1 | 1 |",
		"| [`uris`](reference.md#uri) | `STRING` | Supported |",
		"| [`description`](reference.md#blocked) | `STRING` | Unsupported | Blocked by unsupported setup: `CREATE T`. |",
		"| [`bigtable_options`](reference.md#options) | `STRING` | Partial | Unsupported: `CREATE T OPTIONS (uris = 'https://example.com', bigtable_options = 'b')`; rejected form. |",
	} {
		if !strings.Contains(report, row) {
			t.Errorf("missing %q in report:\n%s", row, report)
		}
	}
}
