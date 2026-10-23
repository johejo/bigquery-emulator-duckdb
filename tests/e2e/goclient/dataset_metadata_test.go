package goclient

import (
	"context"
	"reflect"
	"strings"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
)

// Datasets keep the description, friendly name and labels that datasets.insert, CREATE SCHEMA
// OPTIONS and datasets.patch give them, and forget them when they are dropped. Each has the
// creation time it was created with, which INFORMATION_SCHEMA.SCHEMATA reports too.
func TestDatasetMetadata(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	run := func(sql string) error {
		job, err := client.Query(sql).Run(ctx)
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
	created := map[string]time.Time{}
	check := func(dataset *bigquery.Dataset, want metadata) {
		t.Helper()
		got, err := dataset.Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if g := (metadata{got.Description, got.Name, got.Labels}); !reflect.DeepEqual(g, want) {
			t.Fatalf("%s: got %+v, want %+v", dataset.DatasetID, g, want)
		}
		if first, ok := created[dataset.DatasetID]; got.CreationTime.IsZero() || ok && !got.CreationTime.Equal(first) {
			t.Fatalf("%s: creation time = %v, want %v", dataset.DatasetID, got.CreationTime, first)
		}
		created[dataset.DatasetID] = got.CreationTime
		query := client.Query("SELECT creation_time FROM INFORMATION_SCHEMA.SCHEMATA WHERE schema_name = @name")
		query.Parameters = []bigquery.QueryParameter{{Name: "name", Value: dataset.DatasetID}}
		rows, err := query.Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		var row []bigquery.Value
		if err := rows.Next(&row); err != nil {
			t.Fatal(err)
		}
		if schemata := timeValue(row[0]); !schemata.Equal(got.CreationTime) {
			t.Fatalf("%s: SCHEMATA creation_time = %v, want %v", dataset.DatasetID, schemata, got.CreationTime)
		}
	}

	api := client.Dataset("go_dataset_metadata_api")
	_ = api.DeleteWithContents(ctx)
	t.Cleanup(func() { _ = api.DeleteWithContents(ctx) })
	if err := api.Create(ctx, &bigquery.DatasetMetadata{
		Description: "from the API",
		Name:        "API",
		Labels:      map[string]string{"env": "dev", "team": "data"},
		Location:    "US",
	}); err != nil {
		t.Fatal(err)
	}
	check(api, metadata{"from the API", "API", map[string]string{"env": "dev", "team": "data"}})

	var update bigquery.DatasetMetadataToUpdate
	update.Description = "patched"
	update.SetLabel("tier", "1")
	update.DeleteLabel("env")
	if _, err := api.Update(ctx, update, ""); err != nil {
		t.Fatal(err)
	}
	check(api, metadata{"patched", "API", map[string]string{"team": "data", "tier": "1"}})

	ddl := client.Dataset("go_dataset_metadata_ddl")
	_ = ddl.DeleteWithContents(ctx)
	t.Cleanup(func() { _ = ddl.DeleteWithContents(ctx) })
	create := `CREATE SCHEMA go_dataset_metadata_ddl
		OPTIONS (description = 'from DDL', friendly_name = 'DDL', labels = [('env', 'prod')], location = 'us')`
	if err := run(create); err != nil {
		t.Fatal(err)
	}
	check(ddl, metadata{"from DDL", "DDL", map[string]string{"env": "prod"}})
	// IF NOT EXISTS keeps what is there.
	if err := run("CREATE SCHEMA IF NOT EXISTS go_dataset_metadata_ddl OPTIONS (description = 'other')"); err != nil {
		t.Fatal(err)
	}
	check(ddl, metadata{"from DDL", "DDL", map[string]string{"env": "prod"}})
	// ALTER SCHEMA SET OPTIONS replaces the options it sets, labels included, and leaves the others.
	if err := run("ALTER SCHEMA go_dataset_metadata_ddl SET OPTIONS (description = 'altered', labels = [('tier', '1')])"); err != nil {
		t.Fatal(err)
	}
	check(ddl, metadata{"altered", "DDL", map[string]string{"tier": "1"}})
	if err := run("ALTER SCHEMA go_dataset_metadata_ddl SET OPTIONS (friendly_name = NULL)"); err != nil {
		t.Fatal(err)
	}
	check(ddl, metadata{"altered", "", map[string]string{"tier": "1"}})
	// An empty labels array removes all labels.
	// https://cloud.google.com/bigquery/docs/deleting-labels
	if err := run("ALTER SCHEMA go_dataset_metadata_ddl SET OPTIONS (labels = [])"); err != nil {
		t.Fatal(err)
	}
	check(ddl, metadata{"altered", "", nil})
	if err := run("ALTER SCHEMA IF EXISTS go_dataset_metadata_missing SET OPTIONS (description = 'x')"); err != nil {
		t.Fatal(err)
	}
	// A dataset created again after a drop starts without metadata.
	if err := run("DROP SCHEMA go_dataset_metadata_ddl"); err != nil {
		t.Fatal(err)
	}
	if err := run("CREATE SCHEMA go_dataset_metadata_ddl OPTIONS (labels = [])"); err != nil {
		t.Fatal(err)
	}
	delete(created, ddl.DatasetID)
	check(ddl, metadata{})
	// DROP SCHEMA refuses a dataset that holds tables unless it says CASCADE.
	// https://cloud.google.com/bigquery/docs/reference/standard-sql/data-definition-language#drop_schema_statement
	if err := run("CREATE TABLE go_dataset_metadata_ddl.t (x INT64)"); err != nil {
		t.Fatal(err)
	}
	if err := run("DROP SCHEMA go_dataset_metadata_ddl"); err == nil {
		t.Fatal("DROP SCHEMA dropped a dataset that holds a table")
	}
	if err := run("DROP SCHEMA go_dataset_metadata_ddl CASCADE"); err != nil {
		t.Fatal(err)
	}
	if _, err := ddl.Metadata(ctx); err == nil {
		t.Fatal("DROP SCHEMA CASCADE left the dataset")
	}

	for name, err := range map[string]error{
		"must start with a lowercase letter": client.Dataset("go_dataset_metadata_labeled").Create(ctx, &bigquery.DatasetMetadata{
			Labels: map[string]string{"Env": "prod"},
		}),
		"Not found: Dataset test:go_dataset_metadata_missing": run("ALTER SCHEMA go_dataset_metadata_missing SET OPTIONS (description = 'x')"),
		"Not found: Dataset test:go_dataset_metadata_patched": func() error {
			_, err := client.Dataset("go_dataset_metadata_patched").Update(ctx,
				bigquery.DatasetMetadataToUpdate{Description: "x"}, "")
			return err
		}(),
		// DuckDB's own schemas, where the emulator keeps dataset metadata, are not datasets.
		"Table not found: main.emulator_datasets": run("SELECT * FROM main.emulator_datasets"),
		"Not found: Dataset test:main":            run("DROP TABLE main.emulator_datasets"),
	} {
		if err == nil || !strings.Contains(err.Error(), name) {
			t.Errorf("%s: error = %v", name, err)
		}
	}
}
