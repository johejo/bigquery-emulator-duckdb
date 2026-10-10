package goclient

import (
	"context"
	"reflect"
	"strings"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
)

// Tables and views keep the description, friendly name and labels that tables.insert, DDL OPTIONS
// and tables.patch give them.
func TestTableMetadata(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_table_metadata")
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
	type metadata struct {
		Description, Name string
		Labels            map[string]string
	}
	check := func(name string, want metadata) {
		t.Helper()
		got, err := dataset.Table(name).Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if g := (metadata{got.Description, got.Name, got.Labels}); !reflect.DeepEqual(g, want) {
			t.Fatalf("%s: got %+v, want %+v", name, g, want)
		}
	}

	if err := dataset.Table("api").Create(ctx, &bigquery.TableMetadata{
		Schema:      bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}},
		Description: "from the API",
		Name:        "API",
		Labels:      map[string]string{"env": "dev", "team": "data"},
	}); err != nil {
		t.Fatal(err)
	}
	check("api", metadata{"from the API", "API", map[string]string{"env": "dev", "team": "data"}})

	var update bigquery.TableMetadataToUpdate
	update.Description = "patched"
	update.SetLabel("tier", "1")
	update.DeleteLabel("env")
	if _, err := dataset.Table("api").Update(ctx, update, ""); err != nil {
		t.Fatal(err)
	}
	check("api", metadata{"patched", "API", map[string]string{"team": "data", "tier": "1"}})

	for _, sql := range []string{
		`CREATE TABLE ddl (id INT64 OPTIONS (description = 'key'))
		 OPTIONS (description = 'from DDL', friendly_name = 'DDL', labels = [('env', 'prod')])`,
		`CREATE VIEW v OPTIONS (description = 'a view', labels = [('kind', 'view')])
		 AS SELECT id FROM ddl`,
	} {
		if err := run(sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	check("ddl", metadata{"from DDL", "DDL", map[string]string{"env": "prod"}})
	check("v", metadata{"a view", "", map[string]string{"kind": "view"}})

	// Replacing a table replaces its metadata.
	if err := run("CREATE OR REPLACE TABLE ddl AS SELECT 1 AS id"); err != nil {
		t.Fatal(err)
	}
	check("ddl", metadata{})

	for sql, want := range map[string]string{
		"CREATE TABLE rounded (n NUMERIC OPTIONS (rounding_mode = 'ROUND_HALF_EVEN'))": "column option rounding_mode",
		"CREATE TABLE labeled (id INT64) OPTIONS (labels = [('Env', 'prod')])":         "must start with a lowercase letter",
	} {
		if err := run(sql); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: error = %v, want one containing %q", sql, err, want)
		}
	}
}

// Tables keep the partitioning and clustering that tables.insert and DDL give them. Partitioning
// cannot change, and what the emulator does not emulate is rejected.
func TestTablePartitioning(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_table_partitioning")
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
	schema := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		{Name: "day", Type: bigquery.DateFieldType},
		{Name: "name", Type: bigquery.StringFieldType},
	}
	api := dataset.Table("api")
	if err := api.Create(ctx, &bigquery.TableMetadata{
		Schema:           schema,
		TimePartitioning: &bigquery.TimePartitioning{Type: bigquery.MonthPartitioningType, Field: "day"},
		Clustering:       &bigquery.Clustering{Fields: []string{"name", "id"}},
	}); err != nil {
		t.Fatal(err)
	}
	var update bigquery.TableMetadataToUpdate
	update.Clustering = &bigquery.Clustering{Fields: []string{"id"}}
	metadata, err := api.Update(ctx, update, "")
	if err != nil {
		t.Fatal(err)
	}
	if got, want := metadata.TimePartitioning, (&bigquery.TimePartitioning{Type: bigquery.MonthPartitioningType, Field: "day"}); !reflect.DeepEqual(got, want) {
		t.Errorf("time partitioning = %+v, want %+v", got, want)
	}
	if got, want := metadata.Clustering, (&bigquery.Clustering{Fields: []string{"id"}}); !reflect.DeepEqual(got, want) {
		t.Errorf("clustering = %+v, want %+v", got, want)
	}

	if err := run(`CREATE TABLE ranged (id INT64, name STRING)
		PARTITION BY RANGE_BUCKET(id, GENERATE_ARRAY(0, 100, 10)) CLUSTER BY name`); err != nil {
		t.Fatal(err)
	}
	if metadata, err = dataset.Table("ranged").Metadata(ctx); err != nil {
		t.Fatal(err)
	}
	wantRange := &bigquery.RangePartitioning{Field: "id", Range: &bigquery.RangePartitioningRange{Start: 0, End: 100, Interval: 10}}
	if !reflect.DeepEqual(metadata.RangePartitioning, wantRange) || !reflect.DeepEqual(metadata.Clustering, &bigquery.Clustering{Fields: []string{"name"}}) {
		t.Errorf("range partitioning = %+v, clustering = %+v", metadata.RangePartitioning, metadata.Clustering)
	}

	for name, create := range map[string]func() error{
		"ingestion-time partitioning": func() error {
			return dataset.Table("ingestion").Create(ctx, &bigquery.TableMetadata{
				Schema: schema, TimePartitioning: &bigquery.TimePartitioning{Type: bigquery.DayPartitioningType},
			})
		},
		"partition expiration": func() error {
			return dataset.Table("expiring").Create(ctx, &bigquery.TableMetadata{
				Schema: schema, TimePartitioning: &bigquery.TimePartitioning{Field: "day", Expiration: time.Hour},
			})
		},
		"cannot be found in the schema": func() error {
			return dataset.Table("missing").Create(ctx, &bigquery.TableMetadata{
				Schema: schema, Clustering: &bigquery.Clustering{Fields: []string{"missing"}},
			})
		},
		"require_partition_filter": func() error {
			return run("CREATE TABLE filtered (day DATE) PARTITION BY day OPTIONS (require_partition_filter = TRUE)")
		},
		"clustering for a destination table": func() error {
			query := client.Query("SELECT 1 AS id")
			query.Dst = dataset.Table("clustered")
			query.Clustering = &bigquery.Clustering{Fields: []string{"id"}}
			_, err := query.Run(ctx)
			return err
		},
		"Cannot change the partitioning": func() error {
			var update bigquery.TableMetadataToUpdate
			update.TimePartitioning = &bigquery.TimePartitioning{Type: bigquery.DayPartitioningType, Field: "day"}
			_, err := api.Update(ctx, update, "")
			return err
		},
	} {
		if err := create(); err == nil || !strings.Contains(err.Error(), name) {
			t.Errorf("%s: error = %v", name, err)
		}
	}
}
