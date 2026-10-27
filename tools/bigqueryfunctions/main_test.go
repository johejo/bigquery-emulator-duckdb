package main

import (
	"reflect"
	"strings"
	"testing"
)

const functionsAll = "## Function list\n\n" +
	"| Name | Summary |\n|------|---------|\n" +
	"| [`ABS`](" + base + "/mathematical_functions#abs) | Computes `X` . |\n" +
	"| [`BOOL` (JSON)](" + base + "/json_functions#bool_for_json) | Converts. |\n" +
	"| [`AVG`](" + base + "/aggregate_functions#avg) | Averages. |\n" +
	"| [`AVG`](" + base + "/aggregate-dp-functions#dp_avg) | Averages privately. |\n" +
	"| [`ABS`](" + base + "/mathematical_functions#abs) | Computes `X` . |\n"

const conditionals = "| Name | Summary |\n|------|---------|\n" +
	"| [`CASE expr`](" + base + "/conditional_expressions#case_expr) | Compares. |\n" +
	"| [`IF`](" + base + "/conditional_expressions#if) | Chooses. |\n" +
	"See [`IF`](" + base + "/conditional_expressions#if).\n"

func TestFunctions(t *testing.T) {
	t.Parallel()
	got := functions(functionsAll, conditionals)
	want := []string{
		"ABS mathematical_functions#abs",
		"AVG aggregate-dp-functions#dp_avg",
		"AVG aggregate_functions#avg",
		"BOOL json_functions#bool_for_json",
		"IF conditional_expressions#if",
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("functions() = %q, want %q", got, want)
	}
}

func TestWriteKeepsTheBaseURL(t *testing.T) {
	t.Parallel()
	var b strings.Builder
	if err := write(&b, []string{"ABS mathematical_functions#abs"}); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(b.String(), "# Each line is a function name and its page and anchor under "+base+".") ||
		!strings.HasSuffix(b.String(), "\nABS mathematical_functions#abs\n") {
		t.Errorf("write() = %q", b.String())
	}
}
