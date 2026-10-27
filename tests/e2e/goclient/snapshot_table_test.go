package goclient

import (
	"context"
	"reflect"
	"strings"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// CREATE SNAPSHOT TABLE keeps the rows a table has when it runs in a read-only table snapshot,
// which records the table it was made of and when, and which CREATE TABLE CLONE restores.
// third_party/bigquery_docs/table-snapshots-intro.md
func TestCreateSnapshotTable(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_snapshot_table")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	run := func(sql string) (*bigquery.JobStatus, error) {
		_, status, err := runScript(t, client, dataset.DatasetID, sql)
		return status, err
	}
	mustRun := func(sql string) *bigquery.JobStatus {
		t.Helper()
		status, err := run(sql)
		if err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
		return status
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

	mustRun(`CREATE TABLE base (id INT64 NOT NULL, day DATE) PARTITION BY day
	  OPTIONS (description = 'the base')`)
	mustRun(`INSERT base (id, day) VALUES (1, DATE '2024-01-02'), (2, NULL)`)
	before := time.Now().Add(-time.Second)
	status := mustRun(`CREATE SNAPSHOT TABLE snap CLONE base
	  OPTIONS (friendly_name = 'Snap', labels = [('env', 'dev')])`)
	after := time.Now().Add(time.Second)
	// third_party/bigquery_docs/reference/rest/v2/Job.md
	if got := status.Statistics.Details.(*bigquery.QueryStatistics).StatementType; got != "CREATE_SNAPSHOT_TABLE" {
		t.Errorf("statement type = %q, want CREATE_SNAPSHOT_TABLE", got)
	}
	// The snapshot keeps the rows the table had when it was made.
	mustRun(`INSERT base (id) VALUES (3)`)
	if got, want := ids("snap"), []int64{1, 2}; !reflect.DeepEqual(got, want) {
		t.Errorf("snap: ids = %v, want %v", got, want)
	}

	base, err := dataset.Table("base").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	snap, err := dataset.Table("snap").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if snap.Type != bigquery.Snapshot || !reflect.DeepEqual(snap.Schema, base.Schema) ||
		!reflect.DeepEqual(snap.TimePartitioning, base.TimePartitioning) {
		t.Errorf("snap: type, schema, partitioning = %q, %+v, %+v", snap.Type, snap.Schema, snap.TimePartitioning)
	}
	if snap.Name != "Snap" || !reflect.DeepEqual(snap.Labels, map[string]string{"env": "dev"}) {
		t.Errorf("snap: friendly name, labels = %q, %v", snap.Name, snap.Labels)
	}
	definition := snap.SnapshotDefinition
	if definition == nil || definition.BaseTableReference == nil {
		t.Fatalf("snap: snapshot definition = %+v", definition)
	}
	if got := definition.BaseTableReference; got.ProjectID != client.Project() ||
		got.DatasetID != dataset.DatasetID || got.TableID != "base" {
		t.Errorf("snap: base table = %s.%s.%s", got.ProjectID, got.DatasetID, got.TableID)
	}
	if definition.SnapshotTime.Before(before) || definition.SnapshotTime.After(after) {
		t.Errorf("snap: snapshot time = %v, want between %v and %v", definition.SnapshotTime, before, after)
	}
	if snap.CloneDefinition != nil {
		t.Errorf("snap: clone definition = %+v", snap.CloneDefinition)
	}

	// A snapshot's metadata can change, as a standard table's does.
	// third_party/bigquery_docs/table-snapshots-update.md
	expiration := time.Now().Add(24 * time.Hour).Truncate(time.Millisecond)
	updated, err := dataset.Table("snap").Update(ctx, bigquery.TableMetadataToUpdate{
		Description:    "updated",
		ExpirationTime: expiration,
	}, "")
	if err != nil {
		t.Fatal(err)
	}
	if updated.Description != "updated" || !updated.ExpirationTime.Equal(expiration) ||
		!reflect.DeepEqual(updated.SnapshotDefinition, definition) {
		t.Errorf("updated: description, expiration, snapshot definition = %q, %v, %+v",
			updated.Description, updated.ExpirationTime, updated.SnapshotDefinition)
	}

	// INFORMATION_SCHEMA reports the snapshot, its base table and when it was made.
	// third_party/bigquery_docs/information-schema-tables.md
	// third_party/bigquery_docs/information-schema-snapshots.md
	read := func(sql string) [][]bigquery.Value {
		t.Helper()
		query := client.Query(sql)
		query.DefaultDatasetID = dataset.DatasetID
		rows, err := query.Read(ctx)
		if err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
		var got [][]bigquery.Value
		for {
			var row []bigquery.Value
			if err := rows.Next(&row); err == iterator.Done {
				return got
			} else if err != nil {
				t.Fatal(err)
			}
			got = append(got, row)
		}
	}
	tables := read(`SELECT table_name, table_type, is_insertable_into, base_table_catalog,
	    base_table_schema, base_table_name, snapshot_time_ms = creation_time
	  FROM go_snapshot_table.INFORMATION_SCHEMA.TABLES ORDER BY table_name`)
	if want := [][]bigquery.Value{
		{"base", "BASE TABLE", "YES", nil, nil, nil, nil},
		{"snap", "SNAPSHOT", "NO", client.Project(), dataset.DatasetID, "base", true},
	}; !reflect.DeepEqual(tables, want) {
		t.Errorf("INFORMATION_SCHEMA.TABLES = %v, want %v", tables, want)
	}
	snapshots := read(`SELECT table_name, base_table_catalog, base_table_schema, base_table_name,
	    snapshot_time FROM go_snapshot_table.INFORMATION_SCHEMA.TABLE_SNAPSHOTS`)
	if len(snapshots) != 1 || len(snapshots[0]) != 5 ||
		!reflect.DeepEqual(snapshots[0][:4], []bigquery.Value{"snap", client.Project(), dataset.DatasetID, "base"}) {
		t.Fatalf("INFORMATION_SCHEMA.TABLE_SNAPSHOTS = %v", snapshots)
	}
	if got, ok := snapshots[0][4].(time.Time); !ok || !got.Equal(definition.SnapshotTime) {
		t.Errorf("INFORMATION_SCHEMA.TABLE_SNAPSHOTS snapshot_time = %v, want %v", snapshots[0][4], definition.SnapshotTime)
	}

	// Table snapshots are read-only; scalars/table_snapshots.txt has BigQuery's messages for INSERT,
	// ALTER TABLE and CREATE OR REPLACE TABLE.
	for _, sql := range []string{
		"UPDATE snap SET id = 5 WHERE TRUE",
		"DELETE snap WHERE TRUE",
		"TRUNCATE TABLE snap",
		"MERGE snap USING base ON FALSE WHEN NOT MATCHED THEN INSERT (id) VALUES (id)",
		// BigQuery cannot overwrite a table or a table snapshot with a snapshot, and its syntax
		// has no OR REPLACE for one.
		"CREATE OR REPLACE SNAPSHOT TABLE snap CLONE base",
	} {
		if _, err := run(sql); err == nil {
			t.Errorf("%s: succeeded on a table snapshot", sql)
		}
	}
	// BigQuery's messages for writes through the REST API.
	immutable := "Table " + client.Project() + ":" + dataset.DatasetID + ".snap is a snapshot, and snapshots are immutable."
	wantError := func(what string, err error, want string) {
		t.Helper()
		if err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: error = %v, want one containing %q", what, err, want)
		}
	}
	err = dataset.Table("snap").Inserter().Put(ctx, []*bigquery.ValuesSaver{{
		Schema: snap.Schema,
		Row:    []bigquery.Value{int64(6), nil},
	}})
	wantError("tabledata.insertAll", err, "Cannot add rows to a table of type SNAPSHOT.")
	_, err = dataset.Table("snap").Update(ctx, bigquery.TableMetadataToUpdate{
		Schema: append(snap.Schema, &bigquery.FieldSchema{Name: "extra", Type: bigquery.StringFieldType}),
	}, "")
	wantError("tables.patch of the schema", err,
		"Only table expirationTime, description, friendlyName, labels and tags can be updated for a snapshot.")
	jobError := func(job *bigquery.Job, err error) error {
		if err != nil {
			return err
		}
		status, err := job.Wait(ctx)
		if err != nil {
			return err
		}
		return status.Err()
	}
	for _, disposition := range []bigquery.TableWriteDisposition{bigquery.WriteAppend, bigquery.WriteTruncate} {
		query := client.Query("SELECT 7 AS id, CAST(NULL AS DATE) AS day")
		query.Dst = dataset.Table("snap")
		query.WriteDisposition = disposition
		wantError("query job with "+string(disposition), jobError(query.Run(ctx)), immutable)
	}
	// BigQuery rejects a load when it is inserted, rather than failing the job.
	source := bigquery.NewReaderSource(strings.NewReader(`{"id": 8}` + "\n"))
	source.SourceFormat = bigquery.JSON
	loader := dataset.Table("snap").LoaderFrom(source)
	loader.WriteDisposition = bigquery.WriteAppend
	_, err = loader.Run(ctx)
	wantError("load job", err, immutable)
	if got, want := ids("snap"), []int64{1, 2}; !reflect.DeepEqual(got, want) {
		t.Errorf("snap after rejected writes: ids = %v, want %v", got, want)
	}
	// IF NOT EXISTS keeps the snapshot there.
	mustRun("CREATE SNAPSHOT TABLE IF NOT EXISTS snap CLONE base")
	if got, want := ids("snap"), []int64{1, 2}; !reflect.DeepEqual(got, want) {
		t.Errorf("snap after IF NOT EXISTS: ids = %v, want %v", got, want)
	}

	// CREATE TABLE CLONE restores a snapshot as a standard table, which takes the default
	// expiration of its dataset, here none, rather than the snapshot's.
	// third_party/bigquery_docs/table-snapshots-restore.md
	mustRun("CREATE TABLE restored CLONE snap")
	mustRun("INSERT restored (id) VALUES (8)")
	if got, want := ids("restored"), []int64{1, 2, 8}; !reflect.DeepEqual(got, want) {
		t.Errorf("restored: ids = %v, want %v", got, want)
	}
	restored, err := dataset.Table("restored").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if restored.Type != bigquery.RegularTable || restored.SnapshotDefinition != nil ||
		!restored.ExpirationTime.IsZero() {
		t.Errorf("restored: type, snapshot definition, expiration = %q, %+v, %v",
			restored.Type, restored.SnapshotDefinition, restored.ExpirationTime)
	}
	mustRun("CREATE OR REPLACE TABLE base CLONE snap")
	if got, want := ids("base"), []int64{1, 2}; !reflect.DeepEqual(got, want) {
		t.Errorf("base restored over: ids = %v, want %v", got, want)
	}

	status = mustRun("DROP SNAPSHOT TABLE snap")
	if got := status.Statistics.Details.(*bigquery.QueryStatistics).StatementType; got != "DROP_SNAPSHOT_TABLE" {
		t.Errorf("statement type = %q, want DROP_SNAPSHOT_TABLE", got)
	}
	if _, err := dataset.Table("snap").Metadata(ctx); err == nil {
		t.Error("snap remains after DROP SNAPSHOT TABLE")
	}
	mustRun("DROP SNAPSHOT TABLE IF EXISTS snap")
	// tables.delete deletes a snapshot too.
	// third_party/bigquery_docs/table-snapshots-delete.md
	mustRun("CREATE SNAPSHOT TABLE deleted CLONE base")
	if err := dataset.Table("deleted").Delete(ctx); err != nil {
		t.Errorf("deleting a table snapshot: %v", err)
	}

}
