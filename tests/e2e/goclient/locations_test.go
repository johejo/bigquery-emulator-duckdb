package goclient

import (
	"context"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// Datasets, their tables and the jobs that use them are in a location, as
// https://cloud.google.com/bigquery/docs/locations describes: a job runs where the datasets it
// names are, or where its request says, and every dataset it uses must be there. The project is
// one of its own, so that the datasets of other tests do not show up in INFORMATION_SCHEMA.
func TestLocations(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClientForProject(t, "go-locations")
	create := func(id, location string) *bigquery.Dataset {
		t.Helper()
		dataset := client.Dataset(id)
		_ = dataset.DeleteWithContents(ctx)
		if err := dataset.Create(ctx, &bigquery.DatasetMetadata{Location: location}); err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
		return dataset
	}
	cleanup := func(id string) {
		dataset := client.Dataset(id)
		_ = dataset.DeleteWithContents(ctx)
		t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	}
	eu := create("eu", "EU")
	create("us", "US")
	tokyo := create("tokyo", "asia-northeast1")
	cleanup("ddl_eu")
	cleanup("ddl_job")
	cleanup("ddl_us")
	cleanup("ddl_eu_alone")
	cleanup("ddl_eu_lower")
	cleanup("ddl_eu_implicit")
	cleanup("ddl_tokyo_upper")
	// run runs sql in `location`, or where BigQuery infers it, and returns the job's location.
	run := func(sql, location string) (string, error) {
		t.Helper()
		query := client.Query(sql)
		query.Location = location
		job, err := query.Run(ctx)
		if err != nil {
			return "", err
		}
		status, err := job.Wait(ctx)
		if err != nil {
			return job.Location(), err
		}
		return job.Location(), status.Err()
	}
	for _, sql := range []string{
		"CREATE TABLE eu.t AS SELECT 1 AS x",
		"CREATE TABLE us.t AS SELECT 2 AS x",
		"CREATE TABLE tokyo.t AS SELECT 3 AS x",
	} {
		if _, err := run(sql, ""); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}

	// The dataset's location, which its tables inherit.
	for dataset, want := range map[*bigquery.Dataset]string{eu: "EU", tokyo: "asia-northeast1"} {
		metadata, err := dataset.Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		table, err := dataset.Table("t").Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if metadata.Location != want || table.Location != want {
			t.Errorf("%s: dataset in %q, table in %q, want %q", dataset.DatasetID, metadata.Location,
				table.Location, want)
		}
	}

	// A job runs where the datasets it names are, or where its request says.
	for _, c := range []struct{ sql, location, want string }{
		{"SELECT x FROM eu.t", "", "EU"},
		{"SELECT x FROM tokyo.t", "", "asia-northeast1"},
		{"SELECT 1", "", "US"},
		{"SELECT 1", "EU", "EU"},
		{"SELECT x FROM eu.t", "EU", "EU"},
		{"SELECT COUNT(*) FROM `region-eu`.INFORMATION_SCHEMA.SCHEMATA", "", "EU"},
	} {
		got, err := run(c.sql, c.location)
		if err != nil {
			t.Errorf("%s in %q: %v", c.sql, c.location, err)
		} else if got != c.want {
			t.Errorf("%s in %q: ran in %q, want %q", c.sql, c.location, got, c.want)
		}
	}
	// A job whose names span two locations runs in the US, and names the dataset of the other.
	if _, err := run("SELECT COUNT(*) FROM eu.t, `region-us`.INFORMATION_SCHEMA.SCHEMATA", ""); err == nil ||
		!strings.Contains(err.Error(), "Not found: Dataset go-locations:eu was not found in location US") {
		t.Errorf("a query of EU and US: error = %v", err)
	}
	// Only the prefix of a region qualifier must be in lower case.
	if got, err := run("SELECT COUNT(*) FROM `region-EU`.INFORMATION_SCHEMA.SCHEMATA", ""); err != nil || got != "EU" {
		t.Errorf("region-EU: ran in %q, %v; want EU", got, err)
	}
	// It fails when it uses a dataset elsewhere.
	for _, c := range []struct{ sql, location string }{
		{"SELECT x FROM eu.t UNION ALL SELECT x FROM us.t", ""},
		{"SELECT x FROM eu.t", "US"},
		{"SELECT x FROM tokyo.t", "EU"},
		{"SELECT COUNT(*) FROM `region-eu`.INFORMATION_SCHEMA.SCHEMATA", "US"},
	} {
		if _, err := run(c.sql, c.location); err == nil {
			t.Errorf("%s in %q: succeeded, want an error", c.sql, c.location)
		}
	}

	// CREATE SCHEMA creates a dataset in the location it names, which must be the job's, or else in
	// the job's. A job that names no other dataset runs in the US, so CREATE SCHEMA alone does not
	// settle it, as BigQuery answers.
	if _, err := run("CREATE SCHEMA ddl_eu OPTIONS (location = 'EU')", "EU"); err != nil {
		t.Fatal(err)
	}
	if _, err := run("CREATE SCHEMA ddl_job", "asia-northeast1"); err != nil {
		t.Fatal(err)
	}
	if _, err := run("CREATE SCHEMA ddl_us OPTIONS (location = 'US')", "EU"); err == nil {
		t.Error("CREATE SCHEMA in another location than the job's succeeded, want an error")
	}
	if _, err := run("CREATE SCHEMA ddl_eu_alone OPTIONS (location = 'EU')", ""); err == nil {
		t.Error("CREATE SCHEMA in EU of a job in the US succeeded, want an error")
	}
	// A script runs where its INFORMATION_SCHEMA region is, and a dataset it creates without a
	// location takes the script's; a location is stored as BigQuery spells it.
	for _, sql := range []string{
		"CREATE SCHEMA ddl_eu_lower OPTIONS (location = 'eu'); CREATE SCHEMA ddl_eu_implicit; " +
			"SELECT COUNT(*) FROM `region-eu`.INFORMATION_SCHEMA.SCHEMATA",
		"CREATE SCHEMA ddl_tokyo_upper OPTIONS (location = 'ASIA-NORTHEAST1'); " +
			"SELECT COUNT(*) FROM `region-asia-northeast1`.INFORMATION_SCHEMA.SCHEMATA",
	} {
		if _, err := run(sql, ""); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	// A script whose CREATE SCHEMA names another location than the region it reads fails.
	if _, err := run("CREATE SCHEMA ddl_us OPTIONS (location = 'US'); "+
		"SELECT COUNT(*) FROM `region-eu`.INFORMATION_SCHEMA.SCHEMATA", ""); err == nil {
		t.Error("CREATE SCHEMA in the US of a script in EU succeeded, want an error")
	}
	for id, want := range map[string]string{
		"ddl_eu":          "EU",
		"ddl_job":         "asia-northeast1",
		"ddl_eu_lower":    "EU",
		"ddl_eu_implicit": "EU",
		"ddl_tokyo_upper": "asia-northeast1",
	} {
		metadata, err := client.Dataset(id).Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if metadata.Location != want {
			t.Errorf("%s: in %q, want %q", id, metadata.Location, want)
		}
	}

	// INFORMATION_SCHEMA.SCHEMATA lists the datasets of a region, the US one without a qualifier.
	read := func(sql string) [][]bigquery.Value {
		t.Helper()
		rows, err := client.Query(sql).Read(ctx)
		if err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
		var result [][]bigquery.Value
		for {
			var row []bigquery.Value
			if err := rows.Next(&row); err == iterator.Done {
				return result
			} else if err != nil {
				t.Fatalf("%s: %v", sql, err)
			}
			result = append(result, row)
		}
	}
	for sql, want := range map[string][][]bigquery.Value{
		"SELECT schema_name, location FROM INFORMATION_SCHEMA.SCHEMATA ORDER BY schema_name": {
			{"us", "US"},
		},
		"SELECT schema_name, location FROM `region-eu`.INFORMATION_SCHEMA.SCHEMATA ORDER BY schema_name": {
			{"ddl_eu", "EU"}, {"ddl_eu_implicit", "EU"}, {"ddl_eu_lower", "EU"}, {"eu", "EU"},
		},
		"SELECT schema_name FROM `region-asia-northeast1`.INFORMATION_SCHEMA.SCHEMATA ORDER BY schema_name": {
			{"ddl_job"}, {"ddl_tokyo_upper"}, {"tokyo"},
		},
		// The DDL spells the location in lower case.
		"SELECT REGEXP_EXTRACT(ddl, r'location=\"[^\"]*\"') FROM `region-eu`.INFORMATION_SCHEMA.SCHEMATA WHERE schema_name = 'ddl_eu_lower'": {
			{`location="eu"`},
		},
		"SELECT REGEXP_EXTRACT(ddl, r'location=\"[^\"]*\"') FROM `region-asia-northeast1`.INFORMATION_SCHEMA.SCHEMATA WHERE schema_name = 'ddl_tokyo_upper'": {
			{`location="asia-northeast1"`},
		},
	} {
		if got := read(sql); !reflect.DeepEqual(got, want) {
			t.Errorf("%s: got %v, want %v", sql, got, want)
		}
	}

	// A job outside the US and EU multi-regions is found only by its location.
	query := client.Query("SELECT x FROM tokyo.t")
	job, err := query.Run(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := job.Wait(ctx); err != nil {
		t.Fatal(err)
	}
	if _, err := client.JobFromIDLocation(ctx, job.ID(), "asia-northeast1"); err != nil {
		t.Errorf("job in its location: %v", err)
	}
	for _, location := range []string{"", "US"} {
		if _, err := client.JobFromIDLocation(ctx, job.ID(), location); err == nil {
			t.Errorf("job in %q: found, want not found", location)
		}
	}

	// A load, copy or extract job runs where its tables are.
	copier := eu.Table("copy").CopierFrom(eu.Table("t"))
	copied, err := copier.Run(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if status, err := copied.Wait(ctx); err != nil || status.Err() != nil {
		t.Fatalf("copy: %v, %v", err, status.Err())
	}
	if copied.Location() != "EU" {
		t.Errorf("copy ran in %q, want EU", copied.Location())
	}
	copier = eu.Table("copy_us").CopierFrom(eu.Table("t"))
	copier.Location = "US"
	if copied, err := copier.Run(ctx); err == nil {
		if status, err := copied.Wait(ctx); err == nil && status.Err() == nil {
			t.Error("copy in another location than its tables succeeded, want an error")
		}
	}
}
