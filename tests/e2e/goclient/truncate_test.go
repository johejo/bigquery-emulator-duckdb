package goclient

import (
	"context"
	"reflect"
	"testing"

	"cloud.google.com/go/bigquery"
)

// TRUNCATE TABLE removes every row and keeps the schema. A dry run changes nothing, not even a
// table it would create, and does not read the data, so it validates a DML statement whose rows
// would make it fail.
func TestTruncateTableAndDryRuns(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_truncate")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := datasetQueries{t: t, client: client, dataset: dataset}
	p.mustRun("CREATE TABLE t AS SELECT id, CAST(id AS STRING) AS name FROM UNNEST([1, 2]) AS id")

	for _, sql := range []string{
		"TRUNCATE TABLE t",
		"DELETE FROM t WHERE id = 1",
		"CREATE TABLE dry_run (id INT64)",
		"UPDATE t SET name = s.name FROM UNNEST([STRUCT(1 AS id, 'a' AS name), (1, 'b')]) AS s WHERE t.id = s.id",
	} {
		query := client.Query(sql)
		query.DefaultDatasetID = dataset.DatasetID
		query.DryRun = true
		job, err := query.Run(ctx)
		if err == nil {
			err = job.LastStatus().Err()
		}
		if err != nil {
			t.Errorf("dry run of %s: %v", sql, err)
		}
	}
	want := [][]bigquery.Value{{int64(1), "1"}, {int64(2), "2"}}
	if got := p.read("SELECT id, name FROM t ORDER BY id"); !reflect.DeepEqual(got, want) {
		t.Errorf("rows after dry runs = %v, want %v", got, want)
	}
	if _, err := dataset.Table("dry_run").Metadata(ctx); err == nil {
		t.Error("a dry run of CREATE TABLE created the table")
	}

	p.mustRun("TRUNCATE TABLE `" + client.Project() + ".go_truncate.t`")
	if got := p.read("SELECT id, name FROM t"); len(got) != 0 {
		t.Errorf("rows after TRUNCATE = %v, want none", got)
	}
	p.mustRun("INSERT INTO t VALUES (3, 'three')")
	want = [][]bigquery.Value{{int64(3), "three"}}
	if got := p.read("SELECT id, name FROM t"); !reflect.DeepEqual(got, want) {
		t.Errorf("rows after an insert = %v, want %v", got, want)
	}
}
