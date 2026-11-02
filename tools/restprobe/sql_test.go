package main

import (
	"github.com/johejo/bigquery-emulator-duckdb/internal/probereport"
	"testing"
)

func TestClassifyDisabledSyntax(t *testing.T) {
	t.Parallel()
	for _, message := range []string{
		"INVALID_ARGUMENT: CREATE VIEW with column options not supported [at 1:27]",
		"INVALID_ARGUMENT: COLLATE is not supported [at 1:10]",
	} {
		if got := classify("probe", message); got.Outcome != probereport.Unsupported {
			t.Errorf("classify(%q) = %+v", message, got)
		}
	}
}

func TestClassifyMissingTVF(t *testing.T) {
	t.Parallel()
	for _, message := range []string{
		"INVALID_ARGUMENT: Table-valued function not found: GRAPH_EXPAND [at 1:15]",
		"Table-valued function not found: GRAPH_EXPAND [at 1:92]",
	} {
		got := classify("probe", message)
		if got.Outcome != probereport.Unsupported || got.Detail != "GRAPH_EXPAND" {
			t.Errorf("classify(%q) = %+v", message, got)
		}
	}
}

func TestClassifyInvalidProbe(t *testing.T) {
	t.Parallel()
	for _, message := range []string{
		"INVALID_ARGUMENT: Unknown option: nonexistent",
		"INVALID_ARGUMENT: Syntax error: Unexpected identifier [at 1:1]",
	} {
		if got := classify("probe", message); got.Outcome != probereport.Untested {
			t.Errorf("classify(%q) = %+v", message, got)
		}
	}
}
