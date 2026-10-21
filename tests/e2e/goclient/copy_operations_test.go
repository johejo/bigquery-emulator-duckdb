package goclient

import (
	"context"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// Copy jobs with operationType SNAPSHOT, RESTORE and CLONE make a table snapshot, restore one as a
// standard table, and clone a table, as BigQuery answers.
// https://cloud.google.com/bigquery/docs/reference/rest/v2/Job#OperationType
func TestCopyJobOperationTypes(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_copy_operations")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	for _, sql := range []string{
		`CREATE TABLE base (id INT64, name STRING)
		 OPTIONS (description = 'base', friendly_name = 'Base', labels = [('env', 'dev')])`,
		`INSERT base (id, name) VALUES (1, 'a')`,
		`CREATE SNAPSHOT TABLE snap CLONE base`,
		`CREATE TABLE existing (id INT64, name STRING) OPTIONS (description = 'existing')`,
	} {
		if _, _, err := runScript(t, client, dataset.DatasetID, sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	runCopy := func(source, destination string, operation bigquery.TableCopyOperationType,
		write bigquery.TableWriteDisposition) error {
		copier := dataset.Table(destination).CopierFrom(dataset.Table(source))
		copier.OperationType = operation
		copier.WriteDisposition = write
		job, err := copier.Run(ctx)
		if err != nil {
			return err
		}
		status, err := job.Wait(ctx)
		if err != nil {
			return err
		}
		return status.Err()
	}
	ids := func(table string) []int64 {
		t.Helper()
		query := client.Query("SELECT id FROM " + table + " ORDER BY id")
		query.DefaultDatasetID = dataset.DatasetID
		rows, err := query.Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		var got []int64
		for {
			var row struct{ ID int64 }
			if err := rows.Next(&row); err == iterator.Done {
				return got
			} else if err != nil {
				t.Fatal(err)
			}
			got = append(got, row.ID)
		}
	}
	type described struct {
		Type         bigquery.TableType
		Description  string
		Name         string
		Labels       map[string]string
		SnapshotBase string
		CloneBase    string
	}
	describe := func(table string) described {
		t.Helper()
		metadata, err := dataset.Table(table).Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		got := described{
			Type:        metadata.Type,
			Description: metadata.Description,
			Name:        metadata.Name,
			Labels:      metadata.Labels,
		}
		if definition := metadata.SnapshotDefinition; definition != nil {
			got.SnapshotBase = definition.BaseTableReference.TableID
		}
		if definition := metadata.CloneDefinition; definition != nil {
			got.CloneBase = definition.BaseTableReference.TableID
		}
		return got
	}
	dev := map[string]string{"env": "dev"}

	for _, tc := range []struct {
		source, destination string
		operation           bigquery.TableCopyOperationType
		write               bigquery.TableWriteDisposition
		want                described
	}{
		{"base", "snapshot", bigquery.SnapshotOperation, bigquery.WriteEmpty,
			described{bigquery.Snapshot, "base", "Base", dev, "base", ""}},
		{"snap", "snapshot_of_snapshot", bigquery.SnapshotOperation, bigquery.WriteEmpty,
			described{bigquery.Snapshot, "base", "Base", dev, "snap", ""}},
		{"snap", "restored", bigquery.RestoreOperation, bigquery.WriteEmpty,
			described{bigquery.RegularTable, "base", "Base", dev, "", ""}},
		// A restore that replaces a table keeps that table's metadata.
		{"snap", "existing", bigquery.RestoreOperation, bigquery.WriteTruncate,
			described{bigquery.RegularTable, "existing", "", nil, "", ""}},
		{"base", "cloned", bigquery.CloneOperation, bigquery.WriteEmpty,
			described{bigquery.RegularTable, "base", "Base", dev, "", "base"}},
	} {
		if err := runCopy(tc.source, tc.destination, tc.operation, tc.write); err != nil {
			t.Errorf("%s of %s to %s: %v", tc.operation, tc.source, tc.destination, err)
			continue
		}
		if got := describe(tc.destination); !reflect.DeepEqual(got, tc.want) {
			t.Errorf("%s of %s to %s: %+v, want %+v", tc.operation, tc.source, tc.destination, got, tc.want)
		}
		if got, want := ids(tc.destination), []int64{1}; !reflect.DeepEqual(got, want) {
			t.Errorf("%s of %s to %s: ids = %v, want %v", tc.operation, tc.source, tc.destination, got, want)
		}
	}

	base := client.Project() + ":" + dataset.DatasetID + ".base"
	for _, tc := range []struct {
		source, destination string
		operation           bigquery.TableCopyOperationType
		write               bigquery.TableWriteDisposition
		want                string
	}{
		{"base", "existing", bigquery.SnapshotOperation, bigquery.WriteTruncate,
			"Snapshot operation with write disposition WRITE_TRUNCATE is not supported. Please try again with WRITE_EMPTY."},
		{"base", "restored_table", bigquery.RestoreOperation, bigquery.WriteEmpty,
			base + " is not allowed for this operation because it is currently a TABLE."},
		{"snap", "copied", bigquery.CopyOperation, bigquery.WriteEmpty,
			"Copy operation on SNAPSHOT types is not allowed."},
	} {
		if err := runCopy(tc.source, tc.destination, tc.operation, tc.write); err == nil ||
			!strings.Contains(err.Error(), tc.want) {
			t.Errorf("%s of %s to %s: error = %v, want one containing %q", tc.operation, tc.source,
				tc.destination, err, tc.want)
		}
	}
}
