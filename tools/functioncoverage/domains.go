package main

import (
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"

	"github.com/johejo/bigquery-emulator-duckdb/internal/functioncoverage"
)

// These are reviewed expansions of prose domains in the saved specification, not signatures
// discovered from the emulator. ARRAY/STRUCT domains are necessarily representative: nested
// structural types have infinitely many shapes. Cover ordinary scalar families and nested fields;
// graph and MEASURE values need fixtures and are not included in these domains.
var scalarTypes = []string{
	"INT64", "NUMERIC", "BIGNUMERIC", "FLOAT64", "BOOL", "STRING", "BYTES",
	"DATE", "DATETIME", "TIME", "TIMESTAMP", "INTERVAL", "GEOGRAPHY", "JSON",
	"RANGE<DATE>", "RANGE<DATETIME>", "RANGE<TIMESTAMP>",
}
var structTypes = []string{
	"STRUCT<x INT64, label STRING>",
	"STRUCT<n NUMERIC, nested STRUCT<flag BOOL>>",
	"STRUCT<items ARRAY<BIGNUMERIC>, span RANGE<DATE>>",
}

func proseDomains(root string) ([]functioncoverage.Function, error) {
	functions := map[string]*functioncoverage.Function{}
	pages := map[string]string{}
	var first error
	add := func(page, name string, args []string, result string) *functioncoverage.Signature {
		f := functions[name]
		if f == nil {
			if _, ok := pages[page]; !ok {
				data, err := os.ReadFile(filepath.Join(root, "reference/standard-sql", page+".md"))
				if err != nil && first == nil {
					first = err
				}
				pages[page] = string(data)
			}
			heading := "`" + name + "`\n"
			if !strings.Contains(pages[page], "## "+heading) && !strings.Contains(pages[page], "### "+heading) && first == nil {
				first = fmt.Errorf("%s: missing reviewed function section %s", page, name)
			}
			f = &functioncoverage.Function{Name: name, Source: "third_party/bigquery_docs/reference/standard-sql/" + page + ".md"}
			functions[name] = f
		}
		f.Signatures = append(f.Signatures, functioncoverage.Signature{Arguments: args, Result: result})
		return &f.Signatures[len(f.Signatures)-1]
	}
	for _, t := range []string{"STRING", "BYTES"} {
		for _, name := range []string{"FARM_FINGERPRINT", "MD5", "SHA1", "SHA256", "SHA512"} {
			result := "BYTES"
			if name == "FARM_FINGERPRINT" {
				result = "INT64"
			}
			add("hash_functions", name, []string{t}, result)
		}
		page := "string_functions"
		for _, name := range []string{"ASCII", "BYTE_LENGTH", "OCTET_LENGTH", "LENGTH"} {
			add(page, name, []string{t}, "INT64")
		}
		add(page, "TO_CODE_POINTS", []string{t}, "ARRAY<INT64>")
		for _, name := range []string{"LOWER", "UPPER", "REVERSE"} {
			add(page, name, []string{t}, t)
		}
		for _, name := range []string{"LEFT", "RIGHT", "REPEAT"} {
			add(page, name, []string{t, "INT64"}, t)
		}
		for _, name := range []string{"ENDS_WITH", "STARTS_WITH", "REGEXP_CONTAINS"} {
			add(page, name, []string{t, t}, "BOOL")
		}
		add(page, "STRPOS", []string{t, t}, "INT64")
		for _, name := range []string{"REPLACE", "TRANSLATE", "REGEXP_REPLACE"} {
			add(page, name, []string{t, t, t}, t)
		}
		add(page, "REGEXP_EXTRACT_ALL", []string{t, t}, "ARRAY<"+t+">")
		for _, name := range []string{"SUBSTR", "SUBSTRING"} {
			add(page, name, []string{t, "INT64"}, t)
			add(page, name, []string{t, "INT64", "INT64"}, t)
		}
		for _, name := range []string{"LPAD", "RPAD"} {
			add(page, name, []string{t, "INT64"}, t)
			add(page, name, []string{t, "INT64", t}, t)
		}
		for _, name := range []string{"INSTR", "REGEXP_EXTRACT", "REGEXP_SUBSTR", "REGEXP_INSTR"} {
			result := t
			if name == "INSTR" || name == "REGEXP_INSTR" {
				result = "INT64"
			}
			for n := 2; n <= 4; n++ {
				args := []string{t, t}
				for len(args) < n {
					args = append(args, "INT64")
				}
				add(page, name, args, result)
			}
			if name == "REGEXP_INSTR" {
				s := add(page, name, []string{t, t, "INT64", "INT64", "INT64"}, result)
				s.Samples = []string{"", "", "", "", "1"} // occurrence_position is 0 or 1, not the generic INT64 sample.
			}
		}
		for _, name := range []string{"TRIM", "LTRIM", "RTRIM"} {
			if t == "STRING" {
				add(page, name, []string{t}, t)
			}
			add(page, name, []string{t, t}, t)
		}
		if t == "STRING" {
			add(page, "SPLIT", []string{t}, "ARRAY<"+t+">")
		}
		split := add(page, "SPLIT", []string{t, t}, "ARRAY<"+t+">")
		literal := "'b'"
		if t == "BYTES" {
			literal = "b'b'"
		}
		split.Samples = []string{"", literal}
		split.LiteralArguments = []int{1}
		add(page, "CONCAT", []string{t}, t)
		add(page, "CONCAT", []string{t, t}, t)
		functions["CONCAT"].Note = "Only same-type STRING and BYTES forms are enumerated; implicit conversions to STRING are not yet enumerated."
		add("aggregate_functions", "STRING_AGG", []string{t}, t)
		delimiter := add("aggregate_functions", "STRING_AGG", []string{t, t}, t)
		delimiter.Samples = []string{"", literal}
		delimiter.LiteralArguments = []int{1}
		functions["STRING_AGG"].Note = "The delimiter is sampled as a literal; nonconstant delimiter expressions are not covered."
		add("array_functions", "ARRAY_TO_STRING", []string{"ARRAY<" + t + ">", t}, t)
		add("array_functions", "ARRAY_TO_STRING", []string{"ARRAY<" + t + ">", t, t}, t)
	}
	nonArray := append(slices.Clone(scalarTypes), structTypes...)
	all := slices.Clone(nonArray)
	for _, t := range nonArray {
		all = append(all, "ARRAY<"+t+">")
	}
	for _, t := range nonArray {
		array := "ARRAY<" + t + ">"
		page := "array_functions"
		for _, name := range []string{"ARRAY_FIRST", "ARRAY_LAST"} {
			add(page, name, []string{array}, t)
		}
		add(page, "ARRAY_LENGTH", []string{array}, "INT64")
		add(page, "ARRAY_REVERSE", []string{array}, array)
		add(page, "ARRAY_CONCAT", []string{array}, array)
		add(page, "ARRAY_CONCAT", []string{array, array}, array)
		add(page, "ARRAY_SLICE", []string{array, "INT64", "INT64"}, array)
		add("aggregate_functions", "ARRAY_AGG", []string{t}, array)
		add("aggregate_functions", "ARRAY_CONCAT_AGG", []string{array}, array)
	}
	for _, t := range all {
		add("aggregate_functions", "ANY_VALUE", []string{t}, t)
		add("conditional_expressions", "COALESCE", []string{t}, t)
		add("conditional_expressions", "COALESCE", []string{t, t}, t)
		add("conditional_expressions", "IFNULL", []string{t, t}, t)
		add("conditional_expressions", "IF", []string{"BOOL", t, t}, t)
	}
	for _, name := range []string{"COALESCE", "IFNULL", "IF"} {
		functions[name].Note = "Same-type alternatives are enumerated; mixed-type supertype combinations are not yet enumerated."
	}
	for _, t := range scalarTypes {
		if t == "GEOGRAPHY" || t == "JSON" {
			continue
		} // The specification's orderable-type exclusions.
		for _, name := range []string{"MIN", "MAX"} {
			add("aggregate_functions", name, []string{t}, t)
		}
	}
	numeric := []string{"INT64", "NUMERIC", "BIGNUMERIC", "FLOAT64"}
	for _, t := range numeric {
		for _, name := range []string{"STDDEV", "STDDEV_POP", "STDDEV_SAMP", "VAR_POP", "VAR_SAMP", "VARIANCE"} {
			add("statistical_aggregate_functions", name, []string{t}, "FLOAT64")
		}
		for _, u := range numeric {
			for _, name := range []string{"CORR", "COVAR_POP", "COVAR_SAMP"} {
				add("statistical_aggregate_functions", name, []string{t, u}, "FLOAT64")
			}
		}
		add("array_functions", "GENERATE_ARRAY", []string{t, t}, "ARRAY<"+t+">")
		add("array_functions", "GENERATE_ARRAY", []string{t, t, t}, "ARRAY<"+t+">")
	}
	functions["GENERATE_ARRAY"].Note = "Same-type numeric inputs are enumerated; mixed-type inputs are not yet enumerated."
	for _, t := range []string{"DATE", "DATETIME", "TIMESTAMP"} {
		r := "RANGE<" + t + ">"
		page := "range-functions"
		constructor := add(page, "RANGE", []string{t, t}, r)
		end := "'2024-02-01'"
		if t == "DATETIME" {
			end = "'2024-02-01 00:00:00'"
		}
		if t == "TIMESTAMP" {
			end = "'2024-02-01 00:00:00+00'"
		}
		constructor.Samples = []string{"", end}
		for _, name := range []string{"RANGE_START", "RANGE_END"} {
			add(page, name, []string{r}, t)
		}
		add(page, "RANGE_INTERSECT", []string{r, r}, r)
		add(page, "RANGE_OVERLAPS", []string{r, r}, "BOOL")
		add(page, "RANGE_CONTAINS", []string{r, r}, "BOOL")
		add(page, "RANGE_CONTAINS", []string{r, t}, "BOOL")
	}
	// The generic domains above rely on this saved specification as well as each function page.
	if _, err := os.Stat(filepath.Join(root, "reference/standard-sql/data-types.md")); err != nil && first == nil {
		first = err
	}
	var result []functioncoverage.Function
	for _, f := range functions {
		result = append(result, *f)
	}
	return result, first
}
