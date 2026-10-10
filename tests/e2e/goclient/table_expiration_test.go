package goclient

import (
	"context"
	"fmt"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/googleapi"
	"google.golang.org/api/iterator"
)

// Table.expirationTime documents milliseconds since the epoch, indefinite persistence when
// omitted, and deletion after expiration. DDL expiration_timestamp sets the same property.
// https://cloud.google.com/bigquery/docs/reference/rest/v2/tables
func TestTableExpiration(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_table_expiration")
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
	future := time.Date(2099, 1, 1, 0, 0, 0, 123000000, time.UTC)
	check := func(name string, want time.Time) {
		t.Helper()
		got, err := dataset.Table(name).Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if !got.ExpirationTime.Equal(want) {
			t.Fatalf("%s: expiration = %v, want %v", name, got.ExpirationTime, want)
		}
	}
	for _, view := range []bool{false, true} {
		name := "api_table"
		metadata := &bigquery.TableMetadata{ExpirationTime: future}
		if view {
			name = "api_view"
			metadata.ViewQuery = "SELECT 1 AS id"
		} else {
			metadata.Schema = bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}}
		}
		if err := dataset.Table(name).Create(ctx, metadata); err != nil {
			t.Fatal(err)
		}
		check(name, future)
		if _, err := dataset.Table(name).Update(ctx, bigquery.TableMetadataToUpdate{Description: "kept"}, ""); err != nil {
			t.Fatal(err)
		}
		check(name, future)
		later := future.Add(time.Hour)
		if _, err := dataset.Table(name).Update(ctx, bigquery.TableMetadataToUpdate{ExpirationTime: later}, ""); err != nil {
			t.Fatal(err)
		}
		check(name, later)
		// NeverExpire makes the Go client explicitly clear expiration in a PATCH.
		if _, err := dataset.Table(name).Update(ctx, bigquery.TableMetadataToUpdate{ExpirationTime: bigquery.NeverExpire}, ""); err != nil {
			t.Fatal(err)
		}
		check(name, time.Time{})
	}
	for _, sql := range []string{
		"CREATE TABLE ddl (id INT64) OPTIONS (expiration_timestamp = TIMESTAMP '2099-01-01 00:00:00.123 UTC')",
		"CREATE VIEW v OPTIONS (expiration_timestamp = TIMESTAMP '2099-01-01 00:00:00.123 UTC') AS SELECT 1 AS id",
	} {
		if err := run(sql); err != nil {
			t.Fatal(err)
		}
	}
	check("ddl", future)
	check("v", future)
	for _, form := range []string{"LIKE", "COPY"} {
		name := "inherited_" + form
		if err := run("CREATE TABLE " + name + " " + form + " ddl"); err != nil {
			t.Fatal(err)
		}
		check(name, future)
		if err := run("CREATE TABLE cleared_" + form + " " + form + " ddl OPTIONS (expiration_timestamp = NULL)"); err != nil {
			t.Fatal(err)
		}
		check("cleared_"+form, time.Time{})
	}
	if err := run("ALTER TABLE ddl SET OPTIONS (expiration_timestamp = NULL)"); err != nil {
		t.Fatal(err)
	}
	check("ddl", time.Time{})
	if err := run("ALTER TABLE ddl SET OPTIONS (expiration_timestamp = TIMESTAMP '2099-01-01 00:00:00.123 UTC')"); err != nil {
		t.Fatal(err)
	}
	check("ddl", future)

	// A future deadline must stop an otherwise readable table and view when time passes.
	for _, view := range []bool{false, true} {
		name := "deadline_table"
		deadline := time.Now().Add(2 * time.Second).Truncate(time.Millisecond)
		metadata := &bigquery.TableMetadata{ExpirationTime: deadline}
		if view {
			name = "deadline_view"
			metadata.ViewQuery = "SELECT 1 AS id"
		} else {
			metadata.Schema = bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}}
		}
		if err := dataset.Table(name).Create(ctx, metadata); err != nil {
			t.Fatal(err)
		}
		check(name, deadline)
		time.Sleep(time.Until(deadline.Add(time.Millisecond)))
		if err := run("SELECT * FROM " + name); err == nil {
			t.Fatal("query read a relation after its deadline")
		}
		if _, err := dataset.Table(name).Metadata(ctx); err == nil {
			t.Fatal("metadata returned a relation after its deadline")
		}
	}

	if err := run("ALTER TABLE ddl SET OPTIONS (expiration_timestamp = TIMESTAMP '2000-01-01 00:00:00 UTC')"); err != nil {
		t.Fatal(err)
	}
	if err := run("SELECT * FROM ddl"); err == nil {
		t.Fatal("ALTER TABLE did not expire the table")
	}

	// Setting a past expiration succeeds as a mutation, then subsequent reads see absence.
	if _, err := dataset.Table("api_table").Update(ctx, bigquery.TableMetadataToUpdate{
		ExpirationTime: time.Date(2000, 1, 1, 0, 0, 0, 0, time.UTC),
	}, ""); err != nil {
		t.Fatal(err)
	}
	if _, err := dataset.Table("api_table").Metadata(ctx); err == nil {
		t.Fatal("PATCH did not expire the table")
	}

	for _, access := range []string{"metadata", "query", "rows", "insert", "list", "information_schema", "wildcard", "recreate", "view"} {
		t.Run(access, func(t *testing.T) {
			kind, definition := "TABLE", "(id INT64)"
			if access == "view" {
				kind, definition = "VIEW", "AS SELECT 1 AS id"
			}
			sql := fmt.Sprintf("CREATE %s expired_%s %s", kind, access, definition)
			if access == "view" {
				sql = "CREATE VIEW expired_view OPTIONS (expiration_timestamp = TIMESTAMP '2000-01-01 00:00:00 UTC') AS SELECT 1 AS id"
			} else {
				sql += " OPTIONS (expiration_timestamp = TIMESTAMP '2000-01-01 00:00:00 UTC')"
			}
			if err := run(sql); err != nil {
				t.Fatal(err)
			}
			table := dataset.Table("expired_" + access)
			switch access {
			case "query", "view":
				if err := run("SELECT * FROM " + table.TableID); err == nil {
					t.Fatal("query read an expired relation")
				}
			case "rows":
				var row []bigquery.Value
				if err := table.Read(ctx).Next(&row); err == nil || err == iterator.Done {
					t.Fatalf("read: got %v, want missing table", err)
				}
			case "insert":
				if err := run("INSERT INTO " + table.TableID + " VALUES (1)"); err == nil {
					t.Fatal("insert accepted an expired table")
				}
			case "list":
				it := dataset.Tables(ctx)
				for {
					got, err := it.Next()
					if err == iterator.Done {
						break
					}
					if err != nil {
						t.Fatal(err)
					}
					if got.TableID == table.TableID {
						t.Fatal("list returned an expired table")
					}
				}
			case "information_schema":
				q := client.Query("SELECT table_name FROM " + dataset.DatasetID + ".INFORMATION_SCHEMA.TABLES WHERE table_name = '" + table.TableID + "'")
				it, err := q.Read(ctx)
				if err != nil {
					t.Fatal(err)
				}
				var row []bigquery.Value
				if err := it.Next(&row); err != iterator.Done {
					t.Fatalf("INFORMATION_SCHEMA: got %v, want no rows", err)
				}
			case "wildcard":
				if err := run("SELECT * FROM `" + dataset.DatasetID + ".expired_wild*`"); err == nil {
					t.Fatal("wildcard read an expired table")
				}
			case "recreate":
				if err := table.Create(ctx, &bigquery.TableMetadata{Schema: bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}}}); err != nil {
					t.Fatal(err)
				}
				got, err := table.Metadata(ctx)
				if err != nil || !got.ExpirationTime.IsZero() {
					t.Fatalf("recreated table: got %v, %v", got, err)
				}
				return
			}
			_, err := table.Metadata(ctx)
			apiError, ok := err.(*googleapi.Error)
			if !ok || apiError.Code != 404 {
				t.Fatalf("metadata: got %v, want 404", err)
			}
		})
	}
}
