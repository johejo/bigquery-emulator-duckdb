// Package goclient checks the emulator from the point of view of the Go BigQuery client
// library, which drives the REST API differently from the bq command-line tool: it runs
// parameterised queries, polls jobs and asks for timestamps as epoch microseconds.
package goclient

import (
	"context"
	"errors"
	"os"
	"strings"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/googleapi"
	"google.golang.org/api/iterator"
	"google.golang.org/api/option"
)

func newClient(t *testing.T) *bigquery.Client {
	t.Helper()
	project := os.Getenv("BQ_EMULATOR_PROJECT")
	if project == "" {
		project = "test"
	}
	return newClientForProject(t, project)
}

func newClientForProject(t *testing.T, project string) *bigquery.Client {
	t.Helper()
	endpoint := os.Getenv("BQ_EMULATOR_API")
	if endpoint == "" {
		t.Skip("BQ_EMULATOR_API is not set; run just e2e")
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

func TestInserterPut(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_streaming")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, &bigquery.DatasetMetadata{}); err != nil {
		t.Fatalf("Dataset.Create: %v", err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	table := dataset.Table("events")
	schema := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType, Required: true},
		{Name: "tags", Type: bigquery.StringFieldType, Repeated: true},
		{Name: "details", Type: bigquery.RecordFieldType, Schema: bigquery.Schema{
			{Name: "label", Type: bigquery.StringFieldType},
		}},
	}
	if err := table.Create(ctx, &bigquery.TableMetadata{Schema: schema}); err != nil {
		t.Fatalf("Table.Create: %v", err)
	}
	type details struct {
		Label string `bigquery:"label"`
	}
	type event struct {
		ID      int64    `bigquery:"id"`
		Tags    []string `bigquery:"tags"`
		Details details  `bigquery:"details"`
	}
	if err := table.Inserter().Put(ctx, []*event{{ID: 7, Tags: []string{"a", "b"}, Details: details{Label: "ok"}}}); err != nil {
		t.Fatalf("Inserter.Put: %v", err)
	}
	rows := table.Read(ctx)
	var got struct {
		ID int64
	}
	if err := rows.Next(&got); err != nil {
		t.Fatalf("Next: %v", err)
	}
	if got.ID != 7 {
		t.Errorf("got id %d, want 7", got.ID)
	}
	queryRows, err := client.Query("SELECT id FROM go_streaming.events WHERE id = 7").Read(ctx)
	if err != nil {
		t.Fatalf("Query.Read: %v", err)
	}
	var queried struct{ ID int64 }
	if err := queryRows.Next(&queried); err != nil {
		t.Fatalf("Query.Next: %v", err)
	}
	if queried.ID != 7 {
		t.Errorf("query got id %d, want 7", queried.ID)
	}
}

func TestQueryToADestinationTable(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)

	dataset := client.Dataset("go_destination")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, &bigquery.DatasetMetadata{}); err != nil {
		t.Fatalf("Dataset.Create: %v", err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })

	table := dataset.Table("results")
	for _, disposition := range []bigquery.TableWriteDisposition{
		bigquery.WriteEmpty, bigquery.WriteAppend,
	} {
		query := client.Query("SELECT 1 AS n")
		query.Dst = table
		query.WriteDisposition = disposition
		it, err := query.Read(ctx)
		if err != nil {
			t.Fatalf("Read with %s: %v", disposition, err)
		}
		var row struct{ N int64 }
		if err := it.Next(&row); err != nil || row.N != 1 {
			t.Errorf("got row %+v (%v) with %s, want {1}", row, err, disposition)
		}
	}

	metadata, err := table.Metadata(ctx)
	if err != nil {
		t.Fatalf("Table.Metadata: %v", err)
	}
	if metadata.NumRows != 2 {
		t.Errorf("got %d rows in the destination table, want 2", metadata.NumRows)
	}
}

func TestJobLifecycle(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	query := client.Query("SELECT 1 AS n")
	query.JobID = "go_job_lifecycle"
	job, err := query.Run(ctx)
	if err != nil {
		t.Fatalf("Run: %v", err)
	}
	if err := job.Cancel(ctx); err != nil {
		t.Fatalf("Cancel completed job: %v", err)
	}
	status, err := job.Status(ctx)
	if err != nil || status.State != bigquery.Done || status.Err() != nil {
		t.Fatalf("job after cancel: status=%+v, err=%v", status, err)
	}

	duplicate := client.Query("SELECT 2 AS n")
	duplicate.JobID = query.JobID
	if _, err := duplicate.Run(ctx); err == nil {
		t.Fatal("duplicate job ID was accepted")
	}

	if err := job.Delete(ctx); err != nil {
		t.Fatalf("Delete: %v", err)
	}
	if _, err := client.JobFromID(ctx, query.JobID); err == nil {
		t.Fatal("deleted job is still available")
	}
	reused, err := duplicate.Run(ctx)
	if err != nil {
		t.Fatalf("reuse deleted job ID: %v", err)
	}
	rows, err := reused.Read(ctx)
	if err != nil {
		t.Fatalf("read reused job: %v", err)
	}
	var row struct{ N int64 }
	if err := rows.Next(&row); err != nil || row.N != 2 {
		t.Fatalf("reused job result: row=%+v, err=%v", row, err)
	}
}

func TestJobListAndResultPagination(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	query := client.Query("SELECT x FROM UNNEST([1, 2, 3]) AS x ORDER BY x")
	query.JobID = "go_paginated_job"
	job, err := query.Run(ctx)
	if err != nil {
		t.Fatalf("Run: %v", err)
	}

	results, err := job.Read(ctx)
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	results.PageInfo().MaxSize = 2
	for want := int64(1); want <= 3; want++ {
		var row struct{ X int64 }
		if err := results.Next(&row); err != nil || row.X != want {
			t.Fatalf("result %d: row=%+v, err=%v", want, row, err)
		}
	}
	var row struct{ X int64 }
	if err := results.Next(&row); err != iterator.Done {
		t.Fatalf("after last result: got %v, want iterator.Done", err)
	}
	second := client.Query("SELECT 4 AS x")
	second.JobID = "go_paginated_job_second"
	if _, err := second.Run(ctx); err != nil {
		t.Fatalf("run second job: %v", err)
	}

	jobs := client.Jobs(ctx)
	jobs.State = bigquery.Done
	jobs.PageInfo().MaxSize = 1
	found := map[string]bool{query.JobID: false, second.JobID: false}
	for {
		listed, err := jobs.Next()
		if err == iterator.Done {
			break
		}
		if err != nil {
			t.Fatalf("Jobs.Next: %v", err)
		}
		if _, ok := found[listed.ID()]; ok {
			found[listed.ID()] = true
		}
	}
	if !found[query.JobID] || !found[second.JobID] {
		t.Fatalf("jobs missing from paginated job list: %v", found)
	}
}

func TestStreamingInsertOptions(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_streaming_options")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, &bigquery.DatasetMetadata{}); err != nil {
		t.Fatalf("Dataset.Create: %v", err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })

	table := dataset.Table("events")
	schema := bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType, Required: true}}
	if err := table.Create(ctx, &bigquery.TableMetadata{Schema: schema}); err != nil {
		t.Fatalf("Table.Create: %v", err)
	}
	rows := []*bigquery.ValuesSaver{
		{Schema: schema, Row: []bigquery.Value{int64(1)}},
		{Schema: schema, Row: []bigquery.Value{"invalid"}},
	}
	inserter := table.Inserter()
	var insertionErrors bigquery.PutMultiError
	if err := inserter.Put(ctx, rows); !errors.As(err, &insertionErrors) || len(insertionErrors) != 1 || insertionErrors[0].RowIndex != 1 {
		t.Fatalf("mixed batch errors: %v, want one error for row 1", err)
	}
	checkCount := func(want int64) {
		t.Helper()
		results, err := client.Query("SELECT COUNT(*) AS n FROM go_streaming_options.events").Read(ctx)
		if err != nil {
			t.Fatalf("count query: %v", err)
		}
		var row struct{ N int64 }
		if err := results.Next(&row); err != nil || row.N != want {
			t.Fatalf("row count: got %+v (%v), want %d", row, err, want)
		}
	}
	checkCount(0)

	inserter.SkipInvalidRows = true
	if err := inserter.Put(ctx, rows); !errors.As(err, &insertionErrors) || len(insertionErrors) != 1 || insertionErrors[0].RowIndex != 1 {
		t.Fatalf("skipped row errors: %v, want one error for row 1", err)
	}
	checkCount(1)

	inserter.IgnoreUnknownValues = true
	if err := inserter.Put(ctx, struct {
		ID    int64  `bigquery:"id"`
		Extra string `bigquery:"extra"`
	}{ID: 2, Extra: "ignored"}); err != nil {
		t.Fatalf("insert with unknown field: %v", err)
	}
	checkCount(2)
}

func TestDomainScopedProject(t *testing.T) {
	ctx := context.Background()
	client := newClientForProject(t, "example.com:proj")

	dataset := client.Dataset("go_scoped")
	other := client.Dataset("go_scoped_other")
	_ = dataset.DeleteWithContents(ctx)
	_ = other.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, &bigquery.DatasetMetadata{}); err != nil {
		t.Fatalf("Dataset.Create: %v", err)
	}
	t.Cleanup(func() {
		_ = dataset.DeleteWithContents(ctx)
		_ = other.DeleteWithContents(ctx)
	})
	schema := bigquery.Schema{{Name: "a", Type: bigquery.IntegerFieldType}}
	if err := dataset.Table("t").Create(ctx, &bigquery.TableMetadata{Schema: schema}); err != nil {
		t.Fatalf("Table.Create: %v", err)
	}

	run := func(sql string, withDefaultDataset bool) *bigquery.RowIterator {
		t.Helper()
		query := client.Query(sql)
		if withDefaultDataset {
			query.DefaultProjectID = "example.com:proj"
			query.DefaultDatasetID = "go_scoped"
		}
		rows, err := query.Read(ctx)
		if err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
		return rows
	}
	selectA := func(sql string, withDefaultDataset bool, want int64) {
		t.Helper()
		var row struct{ A int64 }
		if err := run(sql, withDefaultDataset).Next(&row); err != nil || row.A != want {
			t.Errorf("%s: got %+v (%v), want a = %d", sql, row, err, want)
		}
	}

	run("INSERT INTO t (a) VALUES (7)", true)
	selectA("SELECT a FROM t", true, 7)
	selectA("SELECT a FROM go_scoped.t", true, 7)
	selectA("SELECT a FROM go_scoped.t", false, 7)
	selectA("SELECT a FROM `example.com:proj.go_scoped.t`", true, 7)
	selectA("SELECT a FROM `example.com:proj.go_scoped.t`", false, 7)

	run("UPDATE `example.com:proj.go_scoped.t` SET a = 8 WHERE a = 7", false)
	selectA("SELECT a FROM t", true, 8)

	run("CREATE TABLE u AS SELECT a FROM t", true)
	selectA("SELECT a FROM `example.com:proj.go_scoped.u`", false, 8)
	var tables []string
	for it := dataset.Tables(ctx); ; {
		table, err := it.Next()
		if err == iterator.Done {
			break
		}
		if err != nil {
			t.Fatalf("Tables.Next: %v", err)
		}
		tables = append(tables, table.TableID)
	}
	if len(tables) != 2 || tables[0] != "t" || tables[1] != "u" {
		t.Errorf("got tables %v, want [t u]", tables)
	}

	run("CREATE SCHEMA `example.com:proj.go_scoped_other`", false)
	metadata, err := other.Metadata(ctx)
	if err != nil {
		t.Fatalf("created schema: %v", err)
	}
	if metadata.FullID != "example.com:proj:go_scoped_other" {
		t.Errorf("got dataset %q, want example.com:proj:go_scoped_other", metadata.FullID)
	}
}

func TestViewLifecycle(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_views")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	view := dataset.Table("v")
	definition := "SELECT 7 AS id, STRUCT('x' AS name, [1, 2] AS tags) AS nested"
	if err := view.Create(ctx, &bigquery.TableMetadata{ViewQuery: definition}); err != nil {
		t.Fatal(err)
	}
	metadata, err := view.Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if metadata.Type != bigquery.ViewTable || metadata.ViewQuery != definition || metadata.UseLegacySQL {
		t.Fatalf("unexpected view metadata: %+v", metadata)
	}
	if len(metadata.Schema) != 2 || metadata.Schema[0].Type != bigquery.IntegerFieldType ||
		len(metadata.Schema[1].Schema) != 2 || !metadata.Schema[1].Schema[1].Repeated {
		t.Fatalf("unexpected view schema: %+v", metadata.Schema)
	}
	// Duplicate creates must preserve the definition and report the HTTP conflict.
	if err := view.Create(ctx, &bigquery.TableMetadata{ViewQuery: "SELECT 9 AS changed"}); err == nil {
		t.Fatal("duplicate view creation succeeded")
	} else {
		var apiErr *googleapi.Error
		if !errors.As(err, &apiErr) || apiErr.Code != 409 {
			t.Fatalf("expected conflict, got %v", err)
		}
	}
	rows, err := client.Query("SELECT id, nested.name AS name FROM go_views.v").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var row struct {
		ID   int64
		Name string
	}
	if err := rows.Next(&row); err != nil || row.ID != 7 || row.Name != "x" {
		t.Fatalf("view row = %+v, error = %v", row, err)
	}
	if err := view.Delete(ctx); err != nil {
		t.Fatal(err)
	}
	if _, err := view.Metadata(ctx); err == nil {
		t.Fatal("deleted view still exists")
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
