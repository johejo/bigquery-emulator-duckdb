package goclient

import (
	"context"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
)

// Dataset.defaultTableExpirationMs: "all newly-created tables in the dataset will have an
// expirationTime property set to the creation time plus the value in this property, and changing
// the value will only affect new tables, not existing ones", and "an explicit expirationTime when
// creating a table ... takes precedence". The default "affects both tables and views", and DDL's
// default_table_expiration_days sets it in days
// (third_party/bigquery_docs/reference/rest/v2/datasets.md,
// third_party/bigquery_docs/managing-views.md,
// third_party/bigquery_docs/reference/standard-sql/data-definition-language.md).
func TestDatasetDefaultTableExpiration(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	run := func(dataset, sql string) error {
		query := client.Query(sql)
		query.DefaultDatasetID = dataset
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
	mustRun := func(dataset, sql string) {
		t.Helper()
		if err := run(dataset, sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	defaultOf := func(dataset *bigquery.Dataset) time.Duration {
		t.Helper()
		metadata, err := dataset.Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		return metadata.DefaultTableExpiration
	}
	// A table expires its dataset's default after its creation time.
	expiresAfter := func(dataset *bigquery.Dataset, table string, want time.Duration) {
		t.Helper()
		metadata, err := dataset.Table(table).Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if got := metadata.ExpirationTime.Sub(metadata.CreationTime); metadata.ExpirationTime.IsZero() || got != want {
			t.Errorf("%s: expiration = %v, creation = %v; want %v after creation",
				table, metadata.ExpirationTime, metadata.CreationTime, want)
		}
	}
	expiration := func(dataset *bigquery.Dataset, table string) time.Time {
		t.Helper()
		metadata, err := dataset.Table(table).Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		return metadata.ExpirationTime
	}

	api := client.Dataset("go_default_expiration_api")
	_ = api.DeleteWithContents(ctx)
	t.Cleanup(func() { _ = api.DeleteWithContents(ctx) })
	if err := api.Create(ctx, &bigquery.DatasetMetadata{DefaultTableExpiration: 2 * time.Hour}); err != nil {
		t.Fatal(err)
	}
	if got := defaultOf(api); got != 2*time.Hour {
		t.Fatalf("default table expiration = %v, want 2h", got)
	}
	schema := bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}}
	if err := api.Table("rest").Create(ctx, &bigquery.TableMetadata{Schema: schema}); err != nil {
		t.Fatal(err)
	}
	expiresAfter(api, "rest", 2*time.Hour)
	explicit := time.Date(2099, 1, 1, 0, 0, 0, 0, time.UTC)
	if err := api.Table("explicit").Create(ctx, &bigquery.TableMetadata{Schema: schema, ExpirationTime: explicit}); err != nil {
		t.Fatal(err)
	}
	if got := expiration(api, "explicit"); !got.Equal(explicit) {
		t.Errorf("explicit: expiration = %v, want %v", got, explicit)
	}
	mustRun(api.DatasetID, "CREATE TABLE ddl (id INT64)")
	expiresAfter(api, "ddl", 2*time.Hour)
	mustRun(api.DatasetID, "CREATE TABLE ctas AS SELECT 1 AS id")
	expiresAfter(api, "ctas", 2*time.Hour)
	mustRun(api.DatasetID, "CREATE TABLE ddl_explicit (id INT64) OPTIONS (expiration_timestamp = TIMESTAMP '2099-01-01 00:00:00 UTC')")
	if got := expiration(api, "ddl_explicit"); !got.Equal(explicit) {
		t.Errorf("ddl_explicit: expiration = %v, want %v", got, explicit)
	}
	query := client.Query("SELECT 1 AS id")
	query.Dst = api.Table("query")
	job, err := query.Run(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if status, err := job.Wait(ctx); err != nil || status.Err() != nil {
		t.Fatalf("query job: %v, %v", err, status.Err())
	}
	expiresAfter(api, "query", 2*time.Hour)
	// A temporary table belongs to its script, not to the dataset.
	mustRun(api.DatasetID, "CREATE TEMP TABLE scratch (id INT64); INSERT scratch VALUES (1)")

	// Views take the default too; tables.get reports no creation time for a view, so its
	// expiration is checked against the time it was created in.
	before := time.Now()
	if err := api.Table("rest_view").Create(ctx, &bigquery.TableMetadata{ViewQuery: "SELECT 1 AS id"}); err != nil {
		t.Fatal(err)
	}
	mustRun(api.DatasetID, "CREATE VIEW ddl_view AS SELECT 1 AS id")
	after := time.Now()
	for _, view := range []string{"rest_view", "ddl_view"} {
		got := expiration(api, view)
		if got.Before(before.Add(2*time.Hour).Truncate(time.Millisecond)) || got.After(after.Add(2*time.Hour)) {
			t.Errorf("%s: expiration = %v, want 2h after a time in [%v, %v]", view, got, before, after)
		}
	}

	// Changing the default changes it for new tables only.
	var update bigquery.DatasetMetadataToUpdate
	update.DefaultTableExpiration = 3 * time.Hour
	if _, err := api.Update(ctx, update, ""); err != nil {
		t.Fatal(err)
	}
	if got := defaultOf(api); got != 3*time.Hour {
		t.Fatalf("patched default table expiration = %v, want 3h", got)
	}
	expiresAfter(api, "rest", 2*time.Hour)
	mustRun(api.DatasetID, "CREATE TABLE later (id INT64)")
	expiresAfter(api, "later", 3*time.Hour)
	// The Go client sends a zero default as null, which clears it.
	update.DefaultTableExpiration = time.Duration(0)
	if _, err := api.Update(ctx, update, ""); err != nil {
		t.Fatal(err)
	}
	if got := defaultOf(api); got != 0 {
		t.Fatalf("cleared default table expiration = %v", got)
	}
	mustRun(api.DatasetID, "CREATE TABLE never (id INT64)")
	if got := expiration(api, "never"); !got.IsZero() {
		t.Errorf("never: expiration = %v after the default was cleared", got)
	}
	expiresAfter(api, "later", 3*time.Hour)

	// DDL sets the same default in days, which may be fractional.
	ddl := client.Dataset("go_default_expiration_ddl")
	_ = ddl.DeleteWithContents(ctx)
	t.Cleanup(func() { _ = ddl.DeleteWithContents(ctx) })
	mustRun("", "CREATE SCHEMA go_default_expiration_ddl OPTIONS (default_table_expiration_days = 3.75)")
	if got, want := defaultOf(ddl), 90*time.Hour; got != want {
		t.Fatalf("CREATE SCHEMA: default table expiration = %v, want %v", got, want)
	}
	if err := ddl.Table("rest").Create(ctx, &bigquery.TableMetadata{Schema: schema}); err != nil {
		t.Fatal(err)
	}
	expiresAfter(ddl, "rest", 90*time.Hour)
	mustRun("", "ALTER SCHEMA go_default_expiration_ddl SET OPTIONS (default_table_expiration_days = 1)")
	if got := defaultOf(ddl); got != 24*time.Hour {
		t.Fatalf("ALTER SCHEMA: default table expiration = %v, want 24h", got)
	}
	mustRun(ddl.DatasetID, "CREATE TABLE ddl (id INT64)")
	expiresAfter(ddl, "ddl", 24*time.Hour)
	expiresAfter(ddl, "rest", 90*time.Hour)
	mustRun("", "ALTER SCHEMA go_default_expiration_ddl SET OPTIONS (description = 'kept')")
	if got := defaultOf(ddl); got != 24*time.Hour {
		t.Fatalf("ALTER SCHEMA of another option: default table expiration = %v, want 24h", got)
	}
	mustRun("", "ALTER SCHEMA go_default_expiration_ddl SET OPTIONS (default_table_expiration_days = NULL)")
	if got := defaultOf(ddl); got != 0 {
		t.Fatalf("ALTER SCHEMA NULL: default table expiration = %v", got)
	}

	// "The minimum lifetime value is 3600000 milliseconds (one hour)." BigQuery's message is not
	// documented.
	for name, err := range map[string]error{
		"datasets.insert": client.Dataset("go_default_expiration_short").Create(ctx, &bigquery.DatasetMetadata{
			DefaultTableExpiration: time.Hour - time.Millisecond,
		}),
		"CREATE SCHEMA": run("", "CREATE SCHEMA go_default_expiration_short OPTIONS (default_table_expiration_days = 0.04)"),
		"ALTER SCHEMA":  run("", "ALTER SCHEMA go_default_expiration_ddl SET OPTIONS (default_table_expiration_days = -1)"),
	} {
		if err == nil {
			t.Errorf("%s accepted a default table expiration shorter than an hour", name)
		}
	}
	if _, err := client.Dataset("go_default_expiration_short").Metadata(ctx); err == nil {
		t.Error("a dataset with too short a default table expiration was created")
	}
}
