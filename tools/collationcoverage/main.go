// Command collationcoverage lists the operations that the saved BigQuery documentation says
// support collation: the functions, expressions and operators whose sections say so. The resulting
// catalog is checked in so that additions and removals are reviewable, and //:function_probe
// calls each listed function with a collated argument. It does not use the emulator's
// registrations or query BigQuery.
//
//	collationcoverage SNAPSHOT_ROOT
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
)

// Operation is a documented section whose function, expression or operator supports collation.
type Operation struct {
	// The section's heading, such as COUNT, CASE expr or Comparison operators.
	Name   string `json:"name"`
	Source string `json:"source"`
	// Only the DISTINCT form compares under the collation, as for COUNT.
	Distinct bool `json:"distinct,omitempty"`
}

type Catalog struct {
	Operations []Operation `json:"operations"`
}

// A sentence such as "This function supports specifying [collation](...)" or a table row of
// operators that says it.
var supports = regexp.MustCompile(`supports (specifying )?\[collation\]`)

func extract(source, text string) []Operation {
	var operations []Operation
	heading := ""
	for line := range strings.SplitSeq(text, "\n") {
		if strings.HasPrefix(line, "#") {
			heading = strings.Trim(strings.TrimSpace(strings.TrimLeft(line, "#")), "`")
			heading = strings.ReplaceAll(heading, "`", "")
			continue
		}
		if heading == "" || !supports.MatchString(line) {
			continue
		}
		distinct := strings.Contains(line, "with `DISTINCT` supports")
		if len(operations) > 0 && operations[len(operations)-1].Name == heading {
			operations[len(operations)-1].Distinct = operations[len(operations)-1].Distinct && distinct
			continue
		}
		operations = append(operations, Operation{Name: heading, Source: source, Distinct: distinct})
	}
	return operations
}

func generate(root string) (Catalog, error) {
	var c Catalog
	pages, err := filepath.Glob(filepath.Join(root, "reference/standard-sql", "*.md"))
	if err != nil {
		return c, err
	}
	for _, page := range pages {
		// The concepts page describes collation itself rather than an operation.
		if filepath.Base(page) == "collation-concepts.md" {
			continue
		}
		data, err := os.ReadFile(page)
		if err != nil {
			return c, err
		}
		source := "third_party/bigquery_docs/reference/standard-sql/" + filepath.Base(page)
		c.Operations = append(c.Operations, extract(source, string(data))...)
	}
	slices.SortFunc(c.Operations, func(a, b Operation) int {
		return strings.Compare(a.Source+"\x00"+a.Name, b.Source+"\x00"+b.Name)
	})
	if len(c.Operations) == 0 {
		return c, fmt.Errorf("%s: no operation supports collation", root)
	}
	return c, nil
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: collationcoverage SNAPSHOT_ROOT")
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
