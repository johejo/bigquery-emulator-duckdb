// Package functioncoverage describes the documented input and output types of function probes.
// Its catalog is independent of the emulator's function registrations: missing registrations must
// not remove cases from the coverage report.
package functioncoverage

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"slices"
	"strings"
)

type Signature struct {
	Arguments []string `json:"arguments"`
	Result    string   `json:"result"`
	// Samples overrides arguments whose valid values depend on the function, not just the type.
	Samples []string `json:"samples,omitempty"`
	// LiteralArguments names syntax-like options that must stay literals (such as rounding_mode).
	LiteralArguments []int `json:"literal_arguments,omitempty"`
}

type Function struct {
	Name       string      `json:"name"`
	Source     string      `json:"source"`
	Note       string      `json:"note,omitempty"`
	Signatures []Signature `json:"signatures"`
}

type Catalog struct {
	Functions []Function `json:"functions"`
}

func Read(path string) (Catalog, error) {
	f, err := os.Open(path)
	if err != nil {
		return Catalog{}, err
	}
	defer f.Close()
	var c Catalog
	d := json.NewDecoder(f)
	d.DisallowUnknownFields()
	if err := d.Decode(&c); err != nil {
		return c, err
	}
	var extra any
	if err := d.Decode(&extra); err != io.EOF {
		return c, fmt.Errorf("%s: expected one JSON value", path)
	}
	return c, c.Validate()
}

func (c Catalog) Validate() error {
	names := map[string]bool{}
	for _, f := range c.Functions {
		if f.Name == "" || names[f.Name] || f.Name != strings.ToUpper(f.Name) || strings.ContainsAny(f.Name, " ()\n") {
			return fmt.Errorf("invalid or duplicate function: %q", f.Name)
		}
		names[f.Name] = true
		if !strings.HasPrefix(f.Source, "third_party/bigquery_docs/") || strings.Contains(f.Source, "..") || !strings.HasSuffix(f.Source, ".md") {
			return fmt.Errorf("%s: invalid source %q", f.Name, f.Source)
		}
		if len(f.Signatures) == 0 {
			return fmt.Errorf("%s: no signatures", f.Name)
		}
		seen := map[string]bool{}
		for _, s := range f.Signatures {
			key := strings.Join(s.Arguments, ", ")
			if seen[key] || s.Result == "" {
				return fmt.Errorf("%s: duplicate arguments or missing result: %s", f.Name, key)
			}
			seen[key] = true
			if len(s.Samples) != 0 && len(s.Samples) != len(s.Arguments) {
				return fmt.Errorf("%s: samples must match argument count", f.Name)
			}
			for _, i := range s.LiteralArguments {
				if i < 0 || i >= len(s.Samples) || s.Samples[i] == "" {
					return fmt.Errorf("%s: literal argument %d needs a sample", f.Name, i)
				}
			}
			for _, t := range append(slices.Clone(s.Arguments), s.Result) {
				if t == "" || strings.ContainsAny(t, ";\n") {
					return fmt.Errorf("%s: invalid type %q", f.Name, t)
				}
			}
		}
	}
	return nil
}

// Sample returns a typed expression, not an untyped literal that overload resolution can coerce.
// Missing samples are deliberately left Untested by the runner.
func Sample(t string) (string, bool) {
	values := map[string]string{
		"INT64": "2", "NUMERIC": "2", "BIGNUMERIC": "2", "FLOAT64": "2",
		"BOOL": "TRUE", "STRING": "'abc'", "BYTES": "b'abc'",
		"DATE": "'2024-01-15'", "DATETIME": "'2024-01-15 10:20:30'",
		"TIME": "'10:20:30'", "TIMESTAMP": "'2024-01-15 10:20:30+00'",
	}
	if t == "INTERVAL" {
		return "INTERVAL 2 DAY", true
	}
	if t == "GEOGRAPHY" {
		return "ST_GEOGPOINT(1, 2)", true
	}
	if t == "JSON" {
		return `JSON '{"a": [1, 2]}'`, true
	}
	value, ok := values[t]
	if !ok {
		return containerSample(t)
	}
	return "CAST(" + value + " AS " + t + ")", true
}

// Query binds data arguments as columns so literal coercion cannot change their input types.
// Syntax-like options explicitly listed in LiteralArguments remain literals. Query returns a
// reason instead of SQL when a sample is missing. This is not a result-value test.
func Query(f Function, s Signature) (sql, reason string) {
	var columns, args []string
	for i, t := range s.Arguments {
		sample, ok := Sample(t)
		if len(s.Samples) > i && s.Samples[i] != "" {
			sample, ok = "CAST("+s.Samples[i]+" AS "+t+")", true
		}
		if !ok {
			return "", "no sample for " + t
		}
		if slices.Contains(s.LiteralArguments, i) {
			args = append(args, s.Samples[i])
			continue
		}
		name := fmt.Sprintf("a%d", i)
		columns = append(columns, sample+" AS "+name)
		args = append(args, name)
	}
	sql = "SELECT " + f.Name + "(" + strings.Join(args, ", ") + ") AS result"
	if len(columns) > 0 {
		sql += " FROM (SELECT " + strings.Join(columns, ", ") + ")"
	}
	return sql, ""
}
