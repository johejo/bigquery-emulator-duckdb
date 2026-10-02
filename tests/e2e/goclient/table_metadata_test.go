package goclient

import (
	"context"
	"reflect"
	"strings"
	"testing"

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
		"CREATE TABLE expiring (id INT64) OPTIONS (expiration_timestamp = TIMESTAMP '2030-01-01 00:00:00 UTC')": "CREATE TABLE option expiration_timestamp",
		"CREATE TABLE rounded (n NUMERIC OPTIONS (rounding_mode = 'ROUND_HALF_EVEN'))":                          "column option rounding_mode",
		"CREATE TABLE labeled (id INT64) OPTIONS (labels = [('Env', 'prod')])":                                  "must start with a lowercase letter",
	} {
		if err := run(sql); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: error = %v, want one containing %q", sql, err, want)
		}
	}
}
