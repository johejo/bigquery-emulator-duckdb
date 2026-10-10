package goclient

import (
	"context"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

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
