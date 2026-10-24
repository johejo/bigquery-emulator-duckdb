package goclient

import (
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

// COPY of a snapshot remains an emulator limitation; the known-compatible copy
// operation workflows live in scenarios/testdata/copy_operations.api.yaml.
func TestCopyJobFromSnapshotUnsupported(t *testing.T) {
	t.Parallel()
	ctx := t.Context()
	client := newClient(t)
	dataset := client.Dataset("go_copy_snapshot_unsupported")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	if _, _, err := runScript(t, client, dataset.DatasetID,
		"CREATE TABLE base (id INT64); CREATE SNAPSHOT TABLE snap CLONE base"); err != nil {
		t.Fatal(err)
	}
	copier := dataset.Table("copied").CopierFrom(dataset.Table("snap"))
	copier.OperationType = bigquery.CopyOperation
	copier.WriteDisposition = bigquery.WriteEmpty
	job, err := copier.Run(ctx)
	if err == nil {
		var status *bigquery.JobStatus
		status, err = job.Wait(ctx)
		if err == nil {
			err = status.Err()
		}
	}
	if err == nil || !strings.Contains(err.Error(), "Copy operation on SNAPSHOT types is not allowed.") {
		t.Fatalf("COPY of snapshot: error = %v", err)
	}
}
