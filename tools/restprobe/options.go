package main

import (
	"bytes"
	_ "embed"
	"fmt"
	"io"
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

func probeOptions(w io.Writer, binary, path string) error {
	catalog, err := ddloptions.Load(path)
	if err != nil {
		return err
	}
	cases := catalog.Cases()
	type job struct {
		k     int
		query string
	}
	var jobs []job
	for i, k := range cases {
		for _, sql := range k.Queries {
			jobs = append(jobs, job{i, sql})
		}
	}
	probes := make([]probereport.Probe, len(jobs))
	if err := parallel(len(jobs), func(i int) error {
		setup := append(append([]string{}, optionFixture...), cases[jobs[i].k].Context.Setup...)
		var err error
		probes[i], err = probeQuery(binary, setup, jobs[i].query)
		return err
	}); err != nil {
		return err
	}
	results := make([][]probereport.Probe, len(cases))
	for i, j := range jobs {
		p := probes[i]
		if p.Outcome == probereport.Unsupported {
			// Partial rows identify the rejected form, rather than just repeating the option name.
			p.Detail = "`" + j.query + "`"
		}
		results[j.k] = append(results[j.k], p)
	}
	var body, unprobed bytes.Buffer
	var tallies []probereport.Tally
	var current *ddloptions.Context
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
		if k.Context != current {
			current = k.Context
			fmt.Fprintf(&body, "\n## %s\n\n| Option | Type | Status | Notes |\n| --- | --- | --- | --- |\n", current.Name)
			tallies = append(tallies, probereport.Tally{Title: current.Name, Counts: map[string]int{}})
		}
		counts := map[probereport.Outcome]int{}
		rejected := map[string]bool{}
		failures := map[string]bool{}
		for _, p := range results[i] {
			counts[p.Outcome]++
			switch p.Outcome {
			case probereport.Unsupported:
				rejected[p.Detail] = true
			case probereport.Broken:
				failures[p.Detail] = true
			case probereport.Untested:
				return fmt.Errorf("%s.%s: invalid option probe: %s", current.Name, k.Option.Name, p.Detail)
			}
		}
		status := probereport.Status(counts)
		note := ""
		if len(k.Queries) == 0 {
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
