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

// CREATE TABLE COPY and CLONE copy the rows of the source table along with its schema,
// partitioning, clustering and options, which OPTIONS replace. A clone also records the table it
// was cloned from and when.
func TestCreateTableCopyAndClone(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_create_table_copy")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	run := func(sql string) error {
		query := client.Query(sql)
		query.DefaultDatasetID = dataset.DatasetID
		job, err := query.Run(ctx)
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
	for _, sql := range []string{
		`CREATE TABLE source (id INT64 NOT NULL OPTIONS (description = 'key'), day DATE, tags ARRAY<STRING>)
		 PARTITION BY day CLUSTER BY id
		 OPTIONS (description = 'the source', friendly_name = 'Source', labels = [('env', 'dev')])`,
		`INSERT source (id, day, tags) VALUES (1, DATE '2024-01-02', ['a']), (2, NULL, [])`,
		`CREATE TABLE copied COPY source`,
		`CREATE TABLE custom COPY source OPTIONS (description = 'custom')`,
	} {
		if err := run(sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	before := time.Now().Add(-time.Second)
	for _, sql := range []string{
		`CREATE TABLE cloned CLONE source`,
		`CREATE TABLE clone_of_clone CLONE cloned OPTIONS (labels = [('env', 'test')])`,
		`CREATE TABLE copy_of_clone COPY cloned`,
		// The new tables have no relationship to the source table after creation.
		`INSERT source (id) VALUES (3)`,
		`CREATE VIEW v AS SELECT id FROM source`,
		`CREATE TABLE defaulted (a INT64 DEFAULT 1)`,
	} {
		if err := run(sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	after := time.Now().Add(time.Second)
	source, err := dataset.Table("source").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}

	for _, name := range []string{"copied", "cloned", "clone_of_clone", "copy_of_clone"} {
		table, err := dataset.Table(name).Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(table.Schema, source.Schema) {
			t.Errorf("%s: schema = %+v, want %+v", name, table.Schema, source.Schema)
		}
		if !reflect.DeepEqual(table.TimePartitioning, source.TimePartitioning) ||
			!reflect.DeepEqual(table.Clustering, source.Clustering) {
			t.Errorf("%s: partitioning, clustering = %+v, %+v", name, table.TimePartitioning, table.Clustering)
		}
		if got, want := ids(name), []int64{1, 2}; !reflect.DeepEqual(got, want) {
			t.Errorf("%s: ids = %v, want %v", name, got, want)
		}
	}

	copied, err := dataset.Table("copied").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if copied.Description != "the source" || copied.Name != "Source" ||
		!reflect.DeepEqual(copied.Labels, map[string]string{"env": "dev"}) || copied.CloneDefinition != nil {
		t.Errorf("copied: description, name, labels, clone = %q, %q, %v, %+v",
			copied.Description, copied.Name, copied.Labels, copied.CloneDefinition)
	}
	// NOT NULL is copied with the schema.
	if err := run("INSERT copied (day) VALUES (DATE '2024-01-02')"); err == nil {
		t.Error("inserting NULL into a REQUIRED column of the copy succeeded")
	}
	custom, err := dataset.Table("custom").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if custom.Description != "custom" || custom.Name != "Source" ||
		!reflect.DeepEqual(custom.Labels, map[string]string{"env": "dev"}) {
		t.Errorf("custom: description, name, labels = %q, %q, %v", custom.Description, custom.Name, custom.Labels)
	}

	cloned, err := dataset.Table("cloned").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if cloned.Description != "the source" || cloned.Name != "Source" ||
		!reflect.DeepEqual(cloned.Labels, map[string]string{"env": "dev"}) || cloned.Type != bigquery.RegularTable {
		t.Errorf("cloned: description, name, labels, type = %q, %q, %v, %q",
			cloned.Description, cloned.Name, cloned.Labels, cloned.Type)
	}
	clone := cloned.CloneDefinition
	if clone == nil || clone.BaseTableReference == nil {
		t.Fatalf("cloned: clone definition = %+v", clone)
	}
	if got := clone.BaseTableReference; got.ProjectID != client.Project() ||
		got.DatasetID != dataset.DatasetID || got.TableID != "source" {
		t.Errorf("cloned: base table = %s.%s.%s", got.ProjectID, got.DatasetID, got.TableID)
	}
	if clone.CloneTime.Before(before) || clone.CloneTime.After(after) {
		t.Errorf("cloned: clone time = %v, want between %v and %v", clone.CloneTime, before, after)
	}
	cloneOfClone, err := dataset.Table("clone_of_clone").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if got := cloneOfClone.CloneDefinition; got == nil || got.BaseTableReference.TableID != "cloned" ||
		!reflect.DeepEqual(cloneOfClone.Labels, map[string]string{"env": "test"}) {
		t.Errorf("clone_of_clone: clone definition, labels = %+v, %v", got, cloneOfClone.Labels)
	}
	copyOfClone, err := dataset.Table("copy_of_clone").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if copyOfClone.CloneDefinition != nil {
		t.Errorf("copy_of_clone: clone definition = %+v", copyOfClone.CloneDefinition)
	}
	// Updating a clone keeps its clone definition, which clients cannot set.
	updated, err := dataset.Table("cloned").Update(ctx, bigquery.TableMetadataToUpdate{Description: "updated"}, "")
	if err != nil {
		t.Fatal(err)
	}
	if updated.Description != "updated" || !reflect.DeepEqual(updated.CloneDefinition, clone) {
		t.Errorf("updated: description, clone definition = %q, %+v", updated.Description, updated.CloneDefinition)
	}

	query := client.Query(`SELECT table_name, table_type, base_table_catalog, base_table_schema,
	    base_table_name, snapshot_time_ms
	  FROM go_create_table_copy.INFORMATION_SCHEMA.TABLES WHERE table_name IN ('copied', 'cloned') ORDER BY table_name`)
	query.DefaultDatasetID = dataset.DatasetID
	rows, err := query.Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var got [][]bigquery.Value
	for {
		var row []bigquery.Value
		if err := rows.Next(&row); err == iterator.Done {
			break
		} else if err != nil {
			t.Fatal(err)
		}
		got = append(got, row)
	}
	want := [][]bigquery.Value{
		{"cloned", "CLONE", client.Project(), dataset.DatasetID, "source", clone.CloneTime},
		{"copied", "BASE TABLE", nil, nil, nil, nil},
	}
	if len(got) == 2 {
		// The clone time is reported in milliseconds in both.
		if snapshot, ok := got[0][5].(time.Time); ok && snapshot.Equal(clone.CloneTime) {
			got[0][5] = clone.CloneTime
		}
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("INFORMATION_SCHEMA.TABLES = %v, want %v", got, want)
	}

	for sql, want := range map[string]string{
		"CREATE TABLE n COPY v":          "CREATE TABLE COPY a view",
		"CREATE TABLE n CLONE defaulted": "CREATE TABLE CLONE a table with column defaults",
		"CREATE TABLE n COPY source FOR SYSTEM_TIME AS OF CURRENT_TIMESTAMP()":                            "CREATE TABLE COPY with FOR SYSTEM_TIME AS OF",
		"CREATE TABLE n CLONE source FOR SYSTEM_TIME AS OF CURRENT_TIMESTAMP()":                           "CREATE TABLE CLONE with FOR SYSTEM_TIME AS OF",
		"CREATE OR REPLACE TABLE copied CLONE source":                                                     "CREATE OR REPLACE TABLE CLONE",
		"CREATE OR REPLACE TABLE copied COPY copied":                                                      "CREATE OR REPLACE TABLE COPY of itself",
		"CREATE TEMP TABLE n COPY source; SELECT 1":                                                       "CREATE TEMP TABLE COPY",
		"CREATE TABLE n COPY source OPTIONS (expiration_timestamp = TIMESTAMP '2030-01-01 00:00:00 UTC')": "CREATE TABLE option expiration_timestamp",
	} {
		if err := run(sql); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: error = %v, want one containing %q", sql, err, want)
		}
	}
	if got := ids("copied"); !reflect.DeepEqual(got, []int64{1, 2}) {
		t.Errorf("copied after a rejected replacement: ids = %v", got)
	}

	// A copy job records no clone, so it cannot make one.
	copier := dataset.Table("job_clone").CopierFrom(dataset.Table("source"))
	copier.OperationType = bigquery.CloneOperation
	job, err := copier.Run(ctx)
	if err == nil {
		var status *bigquery.JobStatus
		if status, err = job.Wait(ctx); err == nil {
			err = status.Err()
		}
	}
	if err == nil || !strings.Contains(err.Error(), "operationType CLONE") {
		t.Errorf("copy job with operation type CLONE: error = %v", err)
	}
}
