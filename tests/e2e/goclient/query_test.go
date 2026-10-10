package goclient

import (
	"context"
	"strings"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
)

func TestQueryWithNamedParameters(t *testing.T) {
	ctx := context.Background()
	created := time.Date(2024, 1, 2, 3, 4, 5, 0, time.UTC)

	query := newClient(t).Query(
		"SELECT @id AS id, @name AS name, @created AS created, @tags AS tags")
	query.Parameters = []bigquery.QueryParameter{
		{Name: "id", Value: int64(7)},
		{Name: "name", Value: "alice"},
		{Name: "created", Value: created},
		{Name: "tags", Value: []string{"a", "b"}},
	}
	rows, err := query.Read(ctx)
	if err != nil {
		t.Fatalf("Read: %v", err)
	}

	var row struct {
		ID      int64
		Name    string
		Created time.Time
		Tags    []string
	}
	if err := rows.Next(&row); err != nil {
		t.Fatalf("Next: %v", err)
	}
	if row.ID != 7 || row.Name != "alice" {
		t.Errorf("got id %d and name %q, want 7 and \"alice\"", row.ID, row.Name)
	}
	if !row.Created.Equal(created) {
		t.Errorf("got created %s, want %s", row.Created, created)
	}
	if len(row.Tags) != 2 || row.Tags[0] != "a" || row.Tags[1] != "b" {
		t.Errorf("got tags %q, want [a b]", row.Tags)
	}
}

func TestQueryWithPositionalParameters(t *testing.T) {
	ctx := context.Background()
	query := newClient(t).Query("SELECT ? AS a, ? AS b")
	query.Parameters = []bigquery.QueryParameter{{Value: int64(1)}, {Value: "b"}}
	rows, err := query.Read(ctx)
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	var row []bigquery.Value
	if err := rows.Next(&row); err != nil {
		t.Fatalf("Next: %v", err)
	}
	if len(row) != 2 || row[0] != int64(1) || row[1] != "b" {
		t.Errorf("got row %v, want [1 b]", row)
	}
}

func TestDryRunReportsTheSchemaWithoutRunning(t *testing.T) {
	ctx := context.Background()
	query := newClient(t).Query("SELECT 1 AS n, 'x' AS s")
	query.DryRun = true

	job, err := query.Run(ctx)
	if err != nil {
		t.Fatalf("Run: %v", err)
	}
	statistics, ok := job.LastStatus().Statistics.Details.(*bigquery.QueryStatistics)
	if !ok {
		t.Fatalf("got statistics %T, want query statistics", job.LastStatus().Statistics.Details)
	}
	if len(statistics.Schema) != 2 || statistics.Schema[0].Name != "n" {
		t.Errorf("got schema %v, want the two columns of the query", statistics.Schema)
	}
}

// TIMESTAMP and TIME hold microseconds, so a literal with more than six fractional digits is
// invalid, as in BigQuery, rather than truncated.
func TestSubmicrosecondLiterals(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	for sql, want := range map[string]string{
		"SELECT TIMESTAMP '2020-01-01 00:00:00.123456789'": "Invalid TIMESTAMP literal",
		"SELECT TIME '01:02:03.123456789'":                 "Invalid TIME literal",
	} {
		if _, err := client.Query(sql).Read(ctx); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: got error %v, want %q", sql, err, want)
		}
	}
}

// A query that fails while it runs is an error from Read, whether the client gets its results
// from jobs.query or from jobs.getQueryResults, rather than an empty result.
func TestQueryRuntimeError(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	query := client.Query("SELECT ERROR('boom')")
	if _, err := query.Read(ctx); err == nil || !strings.Contains(err.Error(), "boom") {
		t.Errorf("Query.Read: got error %v, want boom", err)
	}
	job, err := query.Run(ctx)
	if err != nil {
		t.Fatalf("Run: %v", err)
	}
	if _, err := job.Read(ctx); err == nil || !strings.Contains(err.Error(), "boom") {
		t.Errorf("Job.Read: got error %v, want boom", err)
	}
}
