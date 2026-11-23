package main

import (
	"bytes"
	_ "embed"
	"errors"
	"fmt"
	"io"
	"slices"
	"strings"

	"github.com/johejo/bigquery-emulator-duckdb/internal/ddloptions"
	"github.com/johejo/bigquery-emulator-duckdb/internal/probereport"
)

//go:embed options_preamble.md
var optionsPreamble string

// optionFixture runs on every fresh emulator before a context's own setup.
var optionFixture = []string{
	"CREATE SCHEMA d",
	"CREATE TABLE d.t (id INT64, d DATE, a NUMERIC) PARTITION BY d",
}

type optionProbe struct {
	probereport.Probe
	blockedBy string
}

func probeOptions(w io.Writer, binary, path string) error {
	catalog, err := ddloptions.Load(path)
	if err != nil {
		return err
	}
	cases := catalog.Cases()
	type job struct {
		k     int
		query ddloptions.Query
	}
	var jobs []job
	for i, k := range cases {
		for _, query := range k.Queries {
			jobs = append(jobs, job{i, query})
		}
	}
	probes := make([]optionProbe, len(jobs))
	if err := parallel(len(jobs), func(i int) error {
		j := jobs[i]
		contextSetup := cases[j.k].Context.Setup
		setup := append(slices.Clone(optionFixture), contextSetup...)
		p, err := probeQuery(binary, setup, j.query.SQL)
		var failed *probeSetupError
		if errors.As(err, &failed) && !slices.Contains(optionFixture, failed.statement) {
			if blocked := classify(failed.statement, failed.message); blocked.Outcome == probereport.Unsupported {
				probes[i] = optionProbe{Probe: blocked, blockedBy: failed.statement}
				return nil
			}
		}
		probes[i] = optionProbe{Probe: p}
		return err
	}); err != nil {
		return err
	}
	results := make([][]optionProbe, len(cases))
	for i, j := range jobs {
		results[j.k] = append(results[j.k], probes[i])
	}
	return writeOptions(w, cases, results)
}

func writeOptions(w io.Writer, cases []ddloptions.Case, results [][]optionProbe) error {
	var body, unprobed bytes.Buffer
	var tallies []probereport.Tally
	var current *ddloptions.Context
	currentHeading := ""
	for i, k := range cases {
		if k.Context.Template == "" {
			// A context without a template has only Untested rows, which a list shows shortly.
			if k.Context != current {
				fmt.Fprintf(&unprobed, "\n- [%s](%s):", k.Context.Name, k.Source)
			} else {
				fmt.Fprint(&unprobed, ",")
			}
			current = k.Context
			fmt.Fprintf(&unprobed, " `%s`", k.Option.Name)
			continue
		}
		current = k.Context
		heading := current.Heading
		if heading == "" {
			heading = current.Name
		}
		if heading != currentHeading {
			currentHeading = heading
			fmt.Fprintf(&body, "\n## %s\n\n| Option | Type | Status | Notes |\n| --- | --- | --- | --- |\n", heading)
			tallies = append(tallies, probereport.Tally{Title: heading, Counts: map[string]int{}})
		}
		counts := map[probereport.Outcome]int{}
		rejected := map[string]bool{}
		failures := map[string]bool{}
		blocked := map[string]bool{}
		for j, p := range results[i] {
			counts[p.Outcome]++
			if p.blockedBy != "" {
				blocked["`"+p.blockedBy+"`"] = true
				continue
			}
			switch p.Outcome {
			case probereport.Unsupported:
				query := k.Queries[j]
				label := query.Label
				if label == "" {
					label = "`" + query.SQL + "`"
				}
				rejected[label] = true
			case probereport.Broken:
				failures[p.Detail] = true
			case probereport.Untested:
				return fmt.Errorf("%s.%s: invalid option probe: %s", current.Name, k.Option.Name, p.Detail)
			}
		}
		status := probereport.Status(counts)
		note := ""
		if len(blocked) != 0 {
			note = "Blocked by unsupported setup: " + strings.Join(probereport.SortedNames(blocked), "; ") + "."
		} else if len(k.Queries) == 0 {
			note = "No sample for this option's constraints."
		}
		tallies[len(tallies)-1].Counts[status]++
		fmt.Fprintf(&body, "| [`%s`](%s) | `%s` | %s | %s |\n", k.Option.Name, k.Source,
			probereport.Escape(k.Option.Type), status,
			strings.TrimSpace(probereport.Notes(note, status, rejected, failures)))
	}
	fmt.Fprint(w, optionsPreamble)
	probereport.WriteSummary(w, "Context", tallies)
	fmt.Fprint(w, body.String())
	if unprobed.Len() > 0 {
		fmt.Fprint(w, "\n## Unprobed contexts\n\nThese contexts have no statement template yet, so none of their options is probed.\n",
			unprobed.String(), "\n")
	}
	return nil
}
