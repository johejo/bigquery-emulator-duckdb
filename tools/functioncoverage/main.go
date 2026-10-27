// Command functioncoverage extracts reviewed classes of type tables from the saved BigQuery
// documentation. The resulting catalog is checked in so additions and removals are reviewable.
// It does not use the emulator's registrations or query BigQuery.
//
//	functioncoverage SNAPSHOT_ROOT
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"

	"github.com/johejo/bigquery-emulator-duckdb/internal/functioncoverage"
)

// cells reads only pipe tables, removing Markdown's inline-code delimiters and padding.
func cells(line string) []string {
	if !strings.HasPrefix(line, "|") || !strings.HasSuffix(line, "|") {
		return nil
	}
	parts := strings.Split(strings.Trim(line, "|"), "|")
	for i := range parts {
		parts[i] = strings.Trim(strings.TrimSpace(parts[i]), "`")
	}
	return parts
}

// tableSignatures recognizes the two documented INPUT table shapes: a unary INPUT/OUTPUT
// table and a binary matrix (the first argument runs down the rows; the second across columns).
// An unrecognized INPUT table fails closed rather than silently losing coverage.
func tableSignatures(rows [][]string) ([]functioncoverage.Signature, error) {
	if len(rows) < 3 || len(rows[0]) < 2 || rows[0][0] != "INPUT" || len(rows[2]) == 0 {
		return nil, fmt.Errorf("incomplete INPUT table")
	}
	header := rows[0]
	if len(rows[1]) != len(header) {
		return nil, fmt.Errorf("invalid table separator")
	}
	for _, cell := range rows[1] {
		if len(strings.Trim(cell, ":")) < 3 || strings.Trim(cell, "-:") != "" {
			return nil, fmt.Errorf("invalid table separator %q", cell)
		}
	}
	var result []functioncoverage.Signature
	if rows[2][0] == "OUTPUT" {
		if len(rows) != 3 || len(rows[2]) != len(header) {
			return nil, fmt.Errorf("invalid INPUT/OUTPUT table")
		}
		for i, t := range header[1:] {
			result = append(result, functioncoverage.Signature{Arguments: []string{t}, Result: rows[2][i+1]})
		}
	} else {
		if len(rows) != len(header)+1 {
			return nil, fmt.Errorf("incomplete input matrix")
		}
		for i, row := range rows[2:] {
			if len(row) != len(header) || row[0] != header[i+1] {
				return nil, fmt.Errorf("invalid input matrix row")
			}
			for j, t := range header[1:] {
				result = append(result, functioncoverage.Signature{Arguments: []string{row[0], t}, Result: row[j+1]})
			}
		}
	}
	for _, s := range result {
		for _, t := range append(slices.Clone(s.Arguments), s.Result) {
			if !slices.Contains([]string{"INT64", "NUMERIC", "BIGNUMERIC", "FLOAT64", "INTERVAL"}, t) {
				return nil, fmt.Errorf("unreviewed table type %q", t)
			}
		}
	}
	return result, nil
}

func extract(page, text string) ([]functioncoverage.Function, error) {
	var functions []functioncoverage.Function
	section := ""
	lines := strings.Split(text, "\n")
	for i := 0; i < len(lines); i++ {
		line := lines[i]
		if strings.HasPrefix(line, "## ") {
			section = strings.Trim(strings.TrimPrefix(line, "## "), "`")
		}
		header := cells(line)
		if len(header) == 0 || header[0] != "INPUT" {
			continue
		}
		if section == "" {
			return nil, fmt.Errorf("INPUT table without function heading")
		}
		rows := [][]string{header}
		for i+1 < len(lines) {
			row := cells(lines[i+1])
			if row == nil {
				break
			}
			rows = append(rows, row)
			i++
		}
		signatures, err := tableSignatures(rows)
		if err != nil {
			return nil, fmt.Errorf("%s/%s: %w", page, section, err)
		}
		functions = append(functions, functioncoverage.Function{
			Name: section, Source: "third_party/bigquery_docs/reference/standard-sql/" + page + ".md",
			Signatures: signatures,
		})
	}
	return functions, nil
}

func generate(root string) (functioncoverage.Catalog, error) {
	var c functioncoverage.Catalog
	// These pages' INPUT tables have been reviewed. Other forms remain explicitly unenumerated
	// in the report; discovering a new shape must not guess which arguments it describes.
	for _, page := range []string{"mathematical_functions", "aggregate_functions"} {
		data, err := os.ReadFile(filepath.Join(root, "reference/standard-sql", page+".md"))
		if err != nil {
			return c, err
		}
		functions, err := extract(page, string(data))
		if err != nil {
			return c, err
		}
		c.Functions = append(c.Functions, functions...)
	}
	// The specification explicitly calls these synonyms; keep their own rows and source pages.
	for _, alias := range [][2]string{{"CEILING", "CEIL"}, {"POWER", "POW"}} {
		for _, f := range c.Functions {
			if f.Name == alias[1] {
				f.Name = alias[0]
				c.Functions = append(c.Functions, f)
				break
			}
		}
	}
	// LOG(X) is documented as LN(X); ROUND and TRUNC take an optional INT64 precision.
	// The rounding_mode argument is valid only for NUMERIC and BIGNUMERIC.
	var ln []functioncoverage.Signature
	for _, f := range c.Functions {
		if f.Name == "LN" {
			ln = f.Signatures
		}
	}
	for i := range c.Functions {
		f := &c.Functions[i]
		switch f.Name {
		case "LOG":
			f.Signatures = append(f.Signatures, ln...)
		case "ROUND", "TRUNC":
			for _, s := range slices.Clone(f.Signatures) {
				s.Arguments = append(slices.Clone(s.Arguments), "INT64")
				f.Signatures = append(f.Signatures, s)
				if f.Name == "ROUND" && slices.Contains([]string{"NUMERIC", "BIGNUMERIC"}, s.Arguments[0]) {
					s.Arguments = append(slices.Clone(s.Arguments), "STRING")
					s.Samples = []string{"", "", "'ROUND_HALF_EVEN'"}
					s.LiteralArguments = []int{2}
					f.Signatures = append(f.Signatures, s)
				}
			}
		}
	}
	slices.SortFunc(c.Functions, func(a, b functioncoverage.Function) int { return strings.Compare(a.Name, b.Name) })
	return c, c.Validate()
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: functioncoverage SNAPSHOT_ROOT")
		os.Exit(2)
	}
	c, err := generate(os.Args[1])
	if err == nil {
		encoder := json.NewEncoder(os.Stdout)
		encoder.SetIndent("", "  ")
		err = encoder.Encode(c)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
