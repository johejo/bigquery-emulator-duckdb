package goclient

import (
	"context"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

// CREATE TABLE ignores NULL option values:
// https://cloud.google.com/bigquery/docs/reference/standard-sql/data-definition-language#create_table_statement
func TestRequirePartitionFilterNullCreateOption(t *testing.T) {
	t.Parallel()
	for _, tc := range []struct {
		name   string
		create string
	}{
		{"literal", "CREATE TABLE filtered (day DATE) PARTITION BY day OPTIONS (require_partition_filter = NULL)"},
		{"expression", "CREATE TABLE filtered (day DATE) PARTITION BY day OPTIONS (require_partition_filter = IF(TRUE, CAST(NULL AS BOOL), TRUE))"},
		{"as_select", "CREATE TABLE filtered PARTITION BY day OPTIONS (require_partition_filter = NULL) AS SELECT DATE '2025-01-01' AS day"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			ctx := context.Background()
			client := newClient(t)
			dataset := client.Dataset("go_partition_filter_null_" + tc.name)
			if err := dataset.Create(ctx, nil); err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
			if _, _, err := runScript(t, client, dataset.DatasetID, tc.create); err != nil {
				t.Fatal(err)
			}
			metadata, err := dataset.Table("filtered").Metadata(ctx)
			if err != nil {
				t.Fatal(err)
			}
			if metadata.RequirePartitionFilter {
				t.Fatal("RequirePartitionFilter = true, want false")
			}
			_, _, err = runScript(t, client, dataset.DatasetID, "SELECT COUNT(*) FROM filtered")
			if err != nil {
				t.Fatal(err)
			}
			if _, _, err := runScript(t, client, dataset.DatasetID, "SELECT COUNT(*) FROM filtered WHERE day = DATE '2025-01-01'"); err != nil {
				t.Fatal(err)
			}
		})
	}
}

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
	if err := run("ALTER TABLE filtered SET OPTIONS (require_partition_filter = NULL)"); err != nil {
		t.Fatal(err)
	}
	check(false)
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
