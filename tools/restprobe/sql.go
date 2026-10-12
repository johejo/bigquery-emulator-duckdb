package main

import (
	"bytes"
	_ "embed"
	"encoding/json"
	"fmt"
	"io"
	"regexp"
	"strings"

	"github.com/johejo/bigquery-emulator-duckdb/internal/probereport"
)

//go:embed sql_preamble.md
var sqlPreamble string

const sqlDataset = "d"

// The analyzer reports errors with a status code such as "INVALID_ARGUMENT: ", which DuckDB's
// errors, such as "Conversion Error: ", and the emulator's own never start with.
var analyzerError = regexp.MustCompile(`^[A-Z_]+: `)

type feature struct {
	name    string
	queries []string
	note    string
}

type section struct {
	title    string
	features []*feature
}

// query runs sql with jobs.query and returns the error it reports, if any.
func query(url, sql, dataset string) (string, bool, error) {
	request := map[string]any{"query": sql, "useLegacySql": false}
	if dataset != "" {
		request["defaultDataset"] = map[string]string{"projectId": "test", "datasetId": dataset}
	}
	body, err := json.Marshal(request)
	if err != nil {
		return "", false, err
	}
	result, err := send(url, "POST", "/bigquery/v2/projects/test/queries", string(body))
	if err != nil {
		return "", false, err
	}
	var decoded map[string]json.RawMessage
	if err := json.Unmarshal(result.body, &decoded); err != nil {
		return "", false, fmt.Errorf("%s: the response is not JSON: %s", sql, result.body)
	}
	message, failed := responseError(decoded)
	return message, failed, nil
}

// probeQuery runs sql on a fresh emulator holding the fixture and classifies the outcome.
func probeQuery(binary string, setup []string, sql string) (probereport.Probe, error) {
	emulator, err := startEmulator(binary)
	if err != nil {
		return probereport.Probe{}, err
	}
	defer emulator.Stop()
	for _, statement := range setup {
		message, failed, err := query(emulator.URL, statement, "")
		if err != nil {
			return probereport.Probe{}, err
		}
		if failed {
			return probereport.Probe{}, fmt.Errorf("setup failed: %s: %s", statement, message)
		}
	}
	message, failed, err := query(emulator.URL, sql, sqlDataset)
	if err != nil || !failed {
		return probereport.Probe{Outcome: probereport.Runs, Detail: ""}, err
	}
	return classify(sql, message), nil
}

// classify names the outcome of sql from the error message jobs.query reported for it.
func classify(sql, message string) probereport.Probe {
	message = firstLine(message)
	if feature, ok := strings.CutPrefix(message, unsupportedPrefix); ok {
		return probereport.Probe{Outcome: probereport.Unsupported, Detail: feature}
	}
	if prefix := analyzerError.FindString(message); prefix != "" {
		// A feature whose language option the emulator leaves off, such as COLLATE, is one it does
		// not support rather than a query the probe got wrong.
		if feature, _, ok := strings.Cut(message[len(prefix):], " is not supported"); ok {
			return probereport.Probe{Outcome: probereport.Unsupported, Detail: feature}
		}
		return probereport.Probe{Outcome: probereport.Untested, Detail: "`" + sql + "`: " + message}
	}
	// DuckDB's hint to add casts is noise in a report.
	message, _, _ = strings.Cut(message, ". You might need")
	return probereport.Probe{Outcome: probereport.Broken, Detail: "`" + sql + "`: " + message}
}

func probeSQL(w io.Writer, binary, path string) error {
	lines, err := readLines(path)
	if err != nil {
		return err
	}
	var setup []string
	var sections []*section
	features := map[string]*feature{}
	for _, l := range lines {
		switch l.directive {
		case "setup":
			setup = append(setup, l.rest)
			continue
		case "section":
			sections = append(sections, &section{title: l.rest})
			continue
		}
		name, value, ok := splitNamed(l.rest)
		if (l.directive != "query" && l.directive != "note") || !ok || len(sections) == 0 {
			return fmt.Errorf("%s:%d: cannot parse: %s %s", path, l.number, l.directive, l.rest)
		}
		f := features[name]
		if f == nil {
			if l.directive == "note" {
				return fmt.Errorf("%s:%d: note before the queries of %s", path, l.number, name)
			}
			f = &feature{name: name}
			features[name] = f
			last := sections[len(sections)-1]
			last.features = append(last.features, f)
		}
		if l.directive == "query" {
			f.queries = append(f.queries, value)
		} else if f.note != "" {
			return fmt.Errorf("%s:%d: second note for %s", path, l.number, name)
		} else {
			f.note = value
		}
	}

	type job struct {
		feature *feature
		sql     string
	}
	var jobs []job
	for _, s := range sections {
		for _, f := range s.features {
			for _, sql := range f.queries {
				jobs = append(jobs, job{f, sql})
			}
		}
	}
	probes := make([]probereport.Probe, len(jobs))
	if err := parallel(len(jobs), func(i int) (err error) {
		probes[i], err = probeQuery(binary, setup, jobs[i].sql)
		return err
	}); err != nil {
		return err
	}
	results := map[*feature][]probereport.Probe{}
	for i, j := range jobs {
		results[j.feature] = append(results[j.feature], probes[i])
	}

	var tallies []probereport.Tally
	var invalid []string
	var body bytes.Buffer
	for _, s := range sections {
		fmt.Fprintf(&body, "## %s\n\n| Feature | Status | Notes |\n| --- | --- | --- |\n", s.title)
		t := probereport.Tally{Title: s.title, Counts: map[string]int{}}
		for _, f := range s.features {
			counts := map[probereport.Outcome]int{}
			rejected := map[string]bool{}
			failures := map[string]bool{}
			for _, p := range results[f] {
				counts[p.Outcome]++
				switch p.Outcome {
				case probereport.Untested:
					invalid = append(invalid, f.name+": "+p.Detail)
				case probereport.Unsupported:
					rejected[p.Detail] = true
				case probereport.Broken:
					failures[p.Detail] = true
				}
			}
			st := probereport.Status(counts)
			t.Counts[st]++
			fmt.Fprintf(&body, "| %s | %s | %s |\n", probereport.Escape(f.name), st,
				probereport.Notes(f.note, st, rejected, failures))
		}
		tallies = append(tallies, t)
		body.WriteString("\n")
	}
	// Every query is written by hand, so one the analyzer rejects is a mistake in the file.
	if len(invalid) > 0 {
		return fmt.Errorf("not valid GoogleSQL:\n%s", strings.Join(invalid, "\n"))
	}
	fmt.Fprint(w, sqlPreamble)
	probereport.WriteSummary(w, "Section", tallies)
	fmt.Fprint(w, "\n", strings.TrimSuffix(body.String(), "\n"))
	return nil
}
