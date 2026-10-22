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
