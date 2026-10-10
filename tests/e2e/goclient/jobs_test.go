package goclient

import (
	"context"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

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
