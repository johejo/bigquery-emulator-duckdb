package goclient

import (
	"context"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

// The documented DDL option and REST field control the same stored requirement.
func TestRequirePartitionFilterMetadata(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_partition_filter_metadata")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	run := func(sql string) error {
		_, _, err := runScript(t, client, dataset.DatasetID, sql)
		return err
	}
	if err := run("CREATE TABLE filtered (day DATE) PARTITION BY day OPTIONS (require_partition_filter = TRUE)"); err != nil {
		t.Fatal(err)
	}
	table := dataset.Table("filtered")
	check := func(want bool) {
		t.Helper()
		metadata, err := table.Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if metadata.RequirePartitionFilter != want {
			t.Fatalf("RequirePartitionFilter = %v, want %v", metadata.RequirePartitionFilter, want)
		}
		err = run("SELECT COUNT(*) FROM " + table.TableID)
		if want {
			if err == nil || !strings.Contains(err.Error(), "that can be used for partition elimination") {
				t.Fatalf("unfiltered query error = %v", err)
			}
		} else if err != nil {
			t.Fatal(err)
		}
	}
	check(true)
	if _, err := table.Update(ctx, bigquery.TableMetadataToUpdate{Description: "keep the filter requirement"}, ""); err != nil {
		t.Fatal(err)
	}
	check(true)
	if err := run("ALTER TABLE filtered SET OPTIONS (require_partition_filter = FALSE)"); err != nil {
		t.Fatal(err)
	}
	check(false)
	if _, err := table.Update(ctx, bigquery.TableMetadataToUpdate{RequirePartitionFilter: true}, ""); err != nil {
		t.Fatal(err)
	}
	check(true)
	if _, err := table.Update(ctx, bigquery.TableMetadataToUpdate{RequirePartitionFilter: false}, ""); err != nil {
		t.Fatal(err)
	}
	check(false)
	if err := run("ALTER TABLE filtered SET OPTIONS (require_partition_filter = TRUE)"); err != nil {
		t.Fatal(err)
	}
	check(true)
	table = dataset.Table("api")
	if err := table.Create(ctx, &bigquery.TableMetadata{
		Schema:                 bigquery.Schema{{Name: "day", Type: bigquery.DateFieldType}},
		TimePartitioning:       &bigquery.TimePartitioning{Type: bigquery.DayPartitioningType, Field: "day"},
		RequirePartitionFilter: true,
	}); err != nil {
		t.Fatal(err)
	}
	check(true)
}
