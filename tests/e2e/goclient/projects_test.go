package goclient

import (
	"context"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

func TestProjectNumberReferences(t *testing.T) {
	ctx := context.Background()
	byNumber := newClientForProject(t, "123456789012")
	byID := newClientForProject(t, "test")
	dataset := byNumber.Dataset("go_project_numbers")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = byID.Dataset(dataset.DatasetID).DeleteWithContents(ctx) })
	if _, err := byID.Dataset(dataset.DatasetID).Metadata(ctx); err != nil {
		t.Fatal(err)
	}
	if err := dataset.Table("t").Create(ctx, &bigquery.TableMetadata{
		Schema: bigquery.Schema{{Name: "value", Type: bigquery.IntegerFieldType}},
	}); err != nil {
		t.Fatal(err)
	}
	if _, err := byID.Dataset(dataset.DatasetID).Table("t").Metadata(ctx); err != nil {
		t.Fatal(err)
	}

	query := byNumber.Query("SELECT 7 AS value")
	query.Dst = dataset.Table("t")
	job, err := query.Run(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if job.ProjectID() != "test" {
		t.Fatalf("job project = %s, want test", job.ProjectID())
	}
	for _, client := range []*bigquery.Client{byNumber, byID} {
		lookup, err := client.JobFromID(ctx, job.ID())
		if err != nil {
			t.Fatal(err)
		}
		status, err := lookup.Wait(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if err := status.Err(); err != nil {
			t.Fatal(err)
		}
	}

	copyJob, err := dataset.Table("copied").CopierFrom(dataset.Table("t")).Run(ctx)
	if err != nil {
		t.Fatal(err)
	}
	status, err := copyJob.Wait(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if err := status.Err(); err != nil {
		t.Fatal(err)
	}

	source := bigquery.NewReaderSource(strings.NewReader("{\"value\":11}\n"))
	source.SourceFormat = bigquery.JSON
	source.Schema = bigquery.Schema{{Name: "value", Type: bigquery.IntegerFieldType}}
	loadJob, err := dataset.Table("loaded").LoaderFrom(source).Run(ctx)
	if err != nil {
		t.Fatal(err)
	}
	status, err = loadJob.Wait(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if err := status.Err(); err != nil {
		t.Fatal(err)
	}

	read := byID.Query("SELECT value FROM copied UNION ALL SELECT value FROM loaded ORDER BY value")
	read.DefaultProjectID = "123456789012"
	read.DefaultDatasetID = dataset.DatasetID
	rows, err := read.Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var row []bigquery.Value
	if err := rows.Next(&row); err != nil {
		t.Fatal(err)
	}
	if len(row) != 1 || row[0] != int64(7) {
		t.Fatalf("row = %v, want [7]", row)
	}
	if err := rows.Next(&row); err != nil {
		t.Fatal(err)
	}
	if len(row) != 1 || row[0] != int64(11) {
		t.Fatalf("row = %v, want [11]", row)
	}
}
