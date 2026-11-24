package main

import (
	"testing"

	"github.com/johejo/bigquery-emulator-duckdb/internal/probereport"
)

func TestClassifyDisabledSyntax(t *testing.T) {
	t.Parallel()
	for _, message := range []string{
		"INVALID_ARGUMENT: CREATE VIEW with column options not supported [at 1:27]",
		"INVALID_ARGUMENT: COLLATE is not supported [at 1:10]",
		"INVALID_ARGUMENT: RESERVATION is not a supported object type [at 1:8]",
		"INVALID_ARGUMENT: REPLICA is not a supported nested object type [at 1:20]",
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

func TestClassifyKnownBigQueryDDL(t *testing.T) {
	t.Parallel()
	for _, tc := range []struct{ sql, message string }{
		{"ALTER SEARCH INDEX i ON t SET OPTIONS (a = 1) REBUILD", "Syntax error: Expected end of input but got keyword REBUILD"},
		{"CREATE TABLE n (x STRING GENERATED ALWAYS AS (AI.EMBED(s)) STORED OPTIONS (asynchronous = TRUE))", "Function not found: AI.EMBED"},
	} {
		message := "INVALID_ARGUMENT: " + tc.message + " [at 1:42]"
		if got := classify(tc.sql, message); got.Outcome != probereport.Unsupported {
			t.Errorf("classify(%q) = %+v", tc.sql, got)
		}
		if got := classify("unrelated probe", message); got.Outcome != probereport.Untested {
			t.Errorf("unrelated probe with %q = %+v", message, got)
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
