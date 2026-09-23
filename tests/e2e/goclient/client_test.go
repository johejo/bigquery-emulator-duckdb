// Package goclient checks the emulator from the point of view of the Go BigQuery client
// library, which drives the REST API differently from the bq command-line tool: it runs
// parameterised queries, polls jobs and asks for timestamps as epoch microseconds.
package goclient

import (
	"context"
	"os"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/option"
)

func newClient(t *testing.T) *bigquery.Client {
	t.Helper()
	endpoint := os.Getenv("BQ_EMULATOR_API")
	if endpoint == "" {
		t.Skip("BQ_EMULATOR_API is not set; start the emulator with tests/e2e/run.sh")
	}
	project := os.Getenv("BQ_EMULATOR_PROJECT")
	if project == "" {
		project = "test"
	}
	client, err := bigquery.NewClient(context.Background(), project,
		option.WithEndpoint(endpoint), option.WithoutAuthentication())
	if err != nil {
		t.Fatalf("bigquery.NewClient: %v", err)
	}
	t.Cleanup(func() { client.Close() })
	return client
}

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

func TestDatasetTableAndRows(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	created := time.Date(2024, 3, 4, 5, 6, 7, 0, time.UTC)

	dataset := client.Dataset("go_e2e")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, &bigquery.DatasetMetadata{}); err != nil {
		t.Fatalf("Dataset.Create: %v", err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })

	table := dataset.Table("users")
	schema := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		{Name: "name", Type: bigquery.StringFieldType},
		{Name: "created", Type: bigquery.TimestampFieldType},
	}
	if err := table.Create(ctx, &bigquery.TableMetadata{Schema: schema}); err != nil {
		t.Fatalf("Table.Create: %v", err)
	}
	metadata, err := table.Metadata(ctx)
	if err != nil {
		t.Fatalf("Table.Metadata: %v", err)
	}
	if len(metadata.Schema) != 3 || metadata.Schema[2].Type != bigquery.TimestampFieldType {
		t.Errorf("got schema %v, want the three columns the table was created with",
			metadata.Schema)
	}

	// A DML statement runs as a job, which the client polls until it is done.
	insert := client.Query("INSERT INTO go_e2e.users VALUES (@id, @name, @created)")
	insert.Parameters = []bigquery.QueryParameter{
		{Name: "id", Value: int64(1)},
		{Name: "name", Value: "alice"},
		{Name: "created", Value: created},
	}
	job, err := insert.Run(ctx)
	if err != nil {
		t.Fatalf("Run: %v", err)
	}
	status, err := job.Wait(ctx)
	if err != nil {
		t.Fatalf("Wait: %v", err)
	}
	if err := status.Err(); err != nil {
		t.Fatalf("insert job: %v", err)
	}

	var row struct {
		ID      int64
		Name    string
		Created time.Time
	}
	rows := table.Read(ctx)
	if err := rows.Next(&row); err != nil {
		t.Fatalf("Next: %v", err)
	}
	if row.ID != 1 || row.Name != "alice" || !row.Created.Equal(created) {
		t.Errorf("got row %+v, want {1 alice %s}", row, created)
	}
}
