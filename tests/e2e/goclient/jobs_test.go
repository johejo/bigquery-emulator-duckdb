package goclient

import (
	"context"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// Keep SDK decoding of creationTime and local-clock bounds in Go. Schema/default
// behavior for the same creation paths is covered by jobs.api.yaml.
func TestJobTableCreationTimeDecoding(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_job_creation_time_decoding")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := creationPaths{t: t, client: client, dataset: dataset}
	schema := bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}, {Name: "tags", Type: bigquery.StringFieldType, Repeated: true}}
	for _, name := range []string{"api", "ddl", "query", "load", "copy"} {
		t.Run(name, func(t *testing.T) {
			before := time.Now().Truncate(time.Millisecond)
			var err error
			switch name {
			case "api":
				err = dataset.Table(name).Create(ctx, &bigquery.TableMetadata{Schema: schema})
			case "ddl":
				err = p.run("CREATE TABLE ddl (id INT64, tags ARRAY<STRING>)")
			case "query":
				err = p.query("SELECT 0 AS id, ['a'] AS tags", name, bigquery.WriteEmpty)
			case "load":
				err = p.load(name, `{"id":0,"tags":["a"]}`, schema, bigquery.WriteEmpty)
			case "copy":
				err = p.copy(name, bigquery.WriteEmpty, "api")
			}
			if err != nil {
				t.Fatal(err)
			}
			metadata, err := dataset.Table(name).Metadata(ctx)
			if err != nil {
				t.Fatal(err)
			}
			if metadata.CreationTime.Before(before) || metadata.CreationTime.After(time.Now()) {
				t.Fatalf("creation time = %v, want a time during creation", metadata.CreationTime)
			}
		})
	}
}

// Query.Read must return the query result even when its job writes a destination.
func TestQueryReadWithDestination(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_destination_sdk")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	for _, write := range []bigquery.TableWriteDisposition{bigquery.WriteEmpty, bigquery.WriteTruncate, bigquery.WriteAppend} {
		query := client.Query("SELECT 1 AS n")
		query.Dst = dataset.Table("results")
		query.WriteDisposition = write
		rows, err := query.Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		var row struct{ N int64 }
		if err := rows.Next(&row); err != nil || row.N != 1 {
			t.Fatalf("Query.Read with %s: row=%+v, err=%v", write, row, err)
		}
	}
}

// SDK result and job iterators must follow page tokens and report iterator.Done.
func TestJobListAndResultPagination(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClientForProject(t, "go-job-list-pages")
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
	// Reproduce the concurrent lifecycle test's deletion between list pages. Make its job
	// newer than ours so that sharing its project again would skip a job after deletion.
	other := newClient(t).Query("SELECT 1")
	other.JobID = "go_paginated_job_unrelated"
	otherJob, err := other.Run(ctx)
	if err != nil {
		t.Fatalf("run job in shared project: %v", err)
	}

	jobs := client.Jobs(ctx)
	jobs.State = bigquery.Done
	jobs.PageInfo().MaxSize = 1
	found := map[string]bool{query.JobID: false, second.JobID: false}
	for page := 0; ; page++ {
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
		if page == 0 {
			if err := otherJob.Delete(ctx); err != nil {
				t.Fatalf("delete job in shared project between pages: %v", err)
			}
		}
	}
	if !found[query.JobID] || !found[second.JobID] {
		t.Fatalf("jobs missing from paginated job list: %v", found)
	}
}
