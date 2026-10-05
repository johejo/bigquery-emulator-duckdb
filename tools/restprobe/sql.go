package main

import (
	"bytes"
	_ "embed"
	"encoding/json"
	"fmt"
	"io"
	"regexp"
	"strings"
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
func probeQuery(binary string, setup []string, sql string) (probe, error) {
	emulator, err := startEmulator(binary)
	if err != nil {
		return probe{}, err
	}
	defer emulator.Stop()
	for _, statement := range setup {
		message, failed, err := query(emulator.URL, statement, "")
		if err != nil {
			return probe{}, err
		}
		if failed {
			return probe{}, fmt.Errorf("setup failed: %s: %s", statement, message)
		}
	}
	message, failed, err := query(emulator.URL, sql, sqlDataset)
	if err != nil || !failed {
		return probe{runs, ""}, err
	}
	message = firstLine(message)
	if feature, ok := strings.CutPrefix(message, unsupportedPrefix); ok {
		return probe{unsupported, feature}, nil
	}
	if prefix := analyzerError.FindString(message); prefix != "" {
		// A feature whose language option the emulator leaves off, such as COLLATE, is one it does
		// not support rather than a query the probe got wrong.
		if feature, _, ok := strings.Cut(message[len(prefix):], " is not supported"); ok {
			return probe{unsupported, feature}, nil
		}
		return probe{untested, "`" + sql + "`: " + message}, nil
	}
	// DuckDB's hint to add casts is noise in a report.
	message, _, _ = strings.Cut(message, ". You might need")
	return probe{broken, "`" + sql + "`: " + message}, nil
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
	probes := make([]probe, len(jobs))
	if err := parallel(len(jobs), func(i int) (err error) {
		probes[i], err = probeQuery(binary, setup, jobs[i].sql)
		return err
	}); err != nil {
		return err
	}
	results := map[*feature][]probe{}
	for i, j := range jobs {
		results[j.feature] = append(results[j.feature], probes[i])
	}

	totals := map[string]int{}
	var invalid []string
	var body bytes.Buffer
	for _, s := range sections {
		fmt.Fprintf(&body, "## %s\n\n| Feature | Status | Notes |\n| --- | --- | --- |\n", s.title)
		for _, f := range s.features {
			counts := map[outcome]int{}
			notes := map[string]bool{}
			for _, p := range results[f] {
				counts[p.outcome]++
				if p.outcome == untested {
					invalid = append(invalid, f.name+": "+p.detail)
				} else if p.outcome != runs {
					notes[p.detail] = true
				}
			}
			st := status(counts)
			totals[st]++
			all := []string{}
			if f.note != "" {
				all = append(all, f.note)
			}
			all = append(all, sortedNames(notes)...)
			fmt.Fprintf(&body, "| %s | %s | %s |\n", escape(f.name), st, escape(strings.Join(all, "; ")))
		}
		body.WriteString("\n")
	}
	// Every query is written by hand, so one the analyzer rejects is a mistake in the file.
	if len(invalid) > 0 {
		return fmt.Errorf("not valid GoogleSQL:\n%s", strings.Join(invalid, "\n"))
	}
	fmt.Fprint(w, sqlPreamble)
	writeTotals(w, "Features", totals)
	fmt.Fprint(w, "\n", strings.TrimSuffix(body.String(), "\n"))
	return nil
}
