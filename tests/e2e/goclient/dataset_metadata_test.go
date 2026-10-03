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
// OPTIONS and datasets.patch give them, and forget them when they are dropped.
func TestDatasetMetadata(t *testing.T) {
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
	check := func(dataset *bigquery.Dataset, want metadata) {
		t.Helper()
		got, err := dataset.Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if g := (metadata{got.Description, got.Name, got.Labels}); !reflect.DeepEqual(g, want) {
			t.Fatalf("%s: got %+v, want %+v", dataset.DatasetID, g, want)
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
	// A dataset created again after a drop starts without metadata.
	if err := run("DROP SCHEMA go_dataset_metadata_ddl"); err != nil {
		t.Fatal(err)
	}
	if err := run("CREATE SCHEMA go_dataset_metadata_ddl"); err != nil {
		t.Fatal(err)
	}
	check(ddl, metadata{})

	for name, err := range map[string]error{
		"dataset location EU": client.Dataset("go_dataset_metadata_eu").Create(ctx, &bigquery.DatasetMetadata{Location: "EU"}),
		"dataset field defaultTableExpirationMs": client.Dataset("go_dataset_metadata_expiring").Create(ctx, &bigquery.DatasetMetadata{
			DefaultTableExpiration: time.Hour,
		}),
		"must start with a lowercase letter": client.Dataset("go_dataset_metadata_labeled").Create(ctx, &bigquery.DatasetMetadata{
			Labels: map[string]string{"Env": "prod"},
		}),
		"CREATE SCHEMA option default_table_expiration_days": run("CREATE SCHEMA go_dataset_metadata_expiring OPTIONS (default_table_expiration_days = 1)"),
		// DuckDB's own schemas, where the emulator keeps dataset metadata, are not datasets.
		"Table not found: main.emulator_datasets": run("SELECT * FROM main.emulator_datasets"),
		"Not found: Dataset test:main":            run("DROP TABLE main.emulator_datasets"),
	} {
		if err == nil || !strings.Contains(err.Error(), name) {
			t.Errorf("%s: error = %v", name, err)
		}
	}
}
