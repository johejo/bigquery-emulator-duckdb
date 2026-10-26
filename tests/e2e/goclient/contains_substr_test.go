package goclient

import (
	"context"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

// Source: BigQuery answers to unverified/contains_substr.txt (2026-10-25).
func TestContainsSubstrSearchParameter(t *testing.T) {
	t.Parallel()
	client := newClient(t)
	for _, c := range []struct {
		name      string
		search    bigquery.NullString
		safe      bool
		wantError string
	}{
		{"string", bigquery.NullString{StringVal: "a", Valid: true}, false, ""},
		{"null", bigquery.NullString{}, false, "The second argument of CONTAINS_SUBSTR() must not be null."},
		{"safe_null", bigquery.NullString{}, true, "SAFE with function contains_substr is not supported."},
	} {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			function := "CONTAINS_SUBSTR"
			if c.safe {
				function = "SAFE." + function
			}
			query := client.Query("SELECT " + function + "('abc', @search) AS found")
			query.Parameters = []bigquery.QueryParameter{{Name: "search", Value: c.search}}
			rows, err := query.Read(context.Background())
			if c.wantError != "" {
				if err == nil || !strings.Contains(err.Error(), c.wantError) {
					t.Fatalf("got %v, want error containing %q", err, c.wantError)
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			var row struct{ Found bool }
			if err := rows.Next(&row); err != nil {
				t.Fatal(err)
			}
			if !row.Found {
				t.Fatal("got false, want true")
			}
			if len(rows.Schema) != 1 || rows.Schema[0].Type != bigquery.BooleanFieldType {
				t.Fatalf("got schema %+v, want BOOL", rows.Schema)
			}
		})
	}
}
