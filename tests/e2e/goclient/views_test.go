package goclient

import (
	"context"
	"errors"
	"reflect"
	"strings"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/googleapi"
	"google.golang.org/api/iterator"
)

func TestViewLifecycle(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_views")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	view := dataset.Table("v")
	definition := "SELECT 7 AS id, STRUCT('x' AS name, [1, 2] AS tags) AS nested"
	before := time.Now().Truncate(time.Millisecond)
	if err := view.Create(ctx, &bigquery.TableMetadata{ViewQuery: definition}); err != nil {
		t.Fatal(err)
	}
	metadata, err := view.Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if metadata.Type != bigquery.ViewTable || metadata.ViewQuery != definition || metadata.UseLegacySQL {
		t.Fatalf("unexpected view metadata: %+v", metadata)
	}
	if len(metadata.Schema) != 2 || metadata.Schema[0].Type != bigquery.IntegerFieldType ||
		len(metadata.Schema[1].Schema) != 2 || !metadata.Schema[1].Schema[1].Repeated {
		t.Fatalf("unexpected view schema: %+v", metadata.Schema)
	}
	// A view has a creation time, which INFORMATION_SCHEMA.TABLES reports too, and updates
	// keep it.
	if metadata.CreationTime.Before(before) || metadata.CreationTime.After(time.Now()) {
		t.Fatalf("creation time = %v, want a time during creation", metadata.CreationTime)
	}
	creationTime := func() time.Time {
		t.Helper()
		query := client.Query("SELECT creation_time FROM go_views.INFORMATION_SCHEMA.TABLES WHERE table_name = 'v'")
		rows, err := query.Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		var row []bigquery.Value
		if err := rows.Next(&row); err != nil {
			t.Fatal(err)
		}
		got, _ := row[0].(time.Time)
		return got
	}
	if got := creationTime(); !got.Equal(metadata.CreationTime) {
		t.Fatalf("INFORMATION_SCHEMA.TABLES creation_time = %v, want %v", got, metadata.CreationTime)
	}
	for _, update := range []bigquery.TableMetadataToUpdate{
		{Description: "the view"},
		{ViewQuery: definition + " LIMIT 1"},
	} {
		updated, err := view.Update(ctx, update, "")
		if err != nil {
			t.Fatal(err)
		}
		if !updated.CreationTime.Equal(metadata.CreationTime) || !creationTime().Equal(metadata.CreationTime) {
			t.Fatalf("update %+v changed creation time from %v to %v", update, metadata.CreationTime, updated.CreationTime)
		}
	}
	// API changes and INFORMATION_SCHEMA.VIEWS describe the same stored definition.
	definitions, err := client.Query("SELECT view_definition, use_standard_sql, check_option FROM go_views.INFORMATION_SCHEMA.VIEWS WHERE table_name = 'v'").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var definitionRow []bigquery.Value
	if err := definitions.Next(&definitionRow); err != nil {
		t.Fatal(err)
	}
	if want := []bigquery.Value{definition + " LIMIT 1", "YES", nil}; !reflect.DeepEqual(definitionRow, want) {
		t.Fatalf("INFORMATION_SCHEMA.VIEWS = %v, want %v", definitionRow, want)
	}
	// Duplicate creates must preserve the definition and report the HTTP conflict.
	if err := view.Create(ctx, &bigquery.TableMetadata{ViewQuery: "SELECT 9 AS changed"}); err == nil {
		t.Fatal("duplicate view creation succeeded")
	} else {
		var apiErr *googleapi.Error
		if !errors.As(err, &apiErr) || apiErr.Code != 409 {
			t.Fatalf("expected conflict, got %v", err)
		}
	}
	rows, err := client.Query("SELECT id, nested.name AS name FROM go_views.v").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var row struct {
		ID   int64
		Name string
	}
	if err := rows.Next(&row); err != nil || row.ID != 7 || row.Name != "x" {
		t.Fatalf("view row = %+v, error = %v", row, err)
	}
	if err := view.Delete(ctx); err != nil {
		t.Fatal(err)
	}
	if _, err := view.Metadata(ctx); err == nil {
		t.Fatal("deleted view still exists")
	}
}

// Views created by DDL keep their query and read their tables when queried; jobs and tabledata
// reject them as destinations or sources.
// third_party/bigquery_docs/views.md
func TestViewStatements(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_view_statements")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	mustRun := func(sql string) {
		t.Helper()
		if _, _, err := runScript(t, client, "", sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	read := func(sql string) []bigquery.Value {
		t.Helper()
		rows, err := client.Query(sql).Read(ctx)
		if err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
		var result []bigquery.Value
		for {
			var row []bigquery.Value
			if err := rows.Next(&row); err == iterator.Done {
				return result
			} else if err != nil {
				t.Fatalf("%s: %v", sql, err)
			}
			result = append(result, row...)
		}
	}
	metadata := func(name string) *bigquery.TableMetadata {
		t.Helper()
		metadata, err := dataset.Table(name).Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		return metadata
	}
	wait := func(job *bigquery.Job, err error) error {
		if err != nil {
			return err
		}
		status, err := job.Wait(ctx)
		if err != nil {
			return err
		}
		return status.Err()
	}
	wantError := func(operation string, err error, want string) {
		t.Helper()
		if err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: got %v, want an error containing %q", operation, err, want)
		}
	}

	mustRun("CREATE TABLE go_view_statements.source AS SELECT 1 AS id")
	mustRun("CREATE VIEW go_view_statements.v (total) AS SELECT SUM(id) FROM go_view_statements.source")
	mustRun("CREATE VIEW go_view_statements.arrays AS SELECT id, [id] AS ids FROM go_view_statements.source")
	const definition = "SELECT SUM(id) FROM go_view_statements.source"
	if got := metadata("v"); got.Type != bigquery.ViewTable || got.ViewQuery != definition ||
		!reflect.DeepEqual(fields(got.Schema), []bigquery.FieldSchema{{Name: "total", Type: bigquery.IntegerFieldType}}) {
		t.Errorf("got view %q of type %v with schema %v", got.ViewQuery, got.Type, fields(got.Schema))
	}
	if got := metadata("arrays").Schema; len(got) != 2 || got[1].Name != "ids" || !got[1].Repeated {
		t.Errorf("got schema %v, want a repeated ids column", fields(got))
	}

	// IF NOT EXISTS keeps the existing definition, which reads the table's current rows.
	mustRun("CREATE VIEW IF NOT EXISTS go_view_statements.v AS SELECT 999 AS ignored")
	mustRun("INSERT INTO go_view_statements.source VALUES (2)")
	if got := read("SELECT * FROM go_view_statements.v"); !reflect.DeepEqual(got, []bigquery.Value{int64(3)}) {
		t.Errorf("got %v, want [3]", got)
	}
	if got := metadata("v").ViewQuery; got != definition {
		t.Errorf("IF NOT EXISTS changed the view to %q", got)
	}

	// A replacement that fails leaves the view as it was.
	const replaced = "SELECT id FROM go_view_statements.source WHERE id > 1"
	mustRun("CREATE OR REPLACE VIEW go_view_statements.v AS " + replaced)
	if _, _, err := runScript(t, client, "",
		"CREATE OR REPLACE VIEW go_view_statements.v AS SELECT missing FROM go_view_statements.source"); err == nil {
		t.Error("replacing a view with an invalid query succeeded")
	}
	if got := metadata("v").ViewQuery; got != replaced {
		t.Errorf("a failed replacement changed the view to %q", got)
	}

	// A view can read another view.
	if err := dataset.Table("nested").Create(ctx, &bigquery.TableMetadata{
		ViewQuery: "SELECT id + 10 AS n FROM go_view_statements.v",
	}); err != nil {
		t.Fatal(err)
	}
	if got := read("SELECT * FROM go_view_statements.nested"); !reflect.DeepEqual(got, []bigquery.Value{int64(12)}) {
		t.Errorf("got %v, want [12]", got)
	}
	if got := read(`SELECT table_type, is_insertable_into, ddl FROM go_view_statements.INFORMATION_SCHEMA.TABLES
		WHERE table_name = 'nested'`); len(got) != 3 || got[0] != "VIEW" || got[1] != "NO" ||
		!strings.HasPrefix(got[2].(string), "CREATE VIEW `") ||
		!strings.HasSuffix(got[2].(string), "AS SELECT id + 10 AS n FROM go_view_statements.v;") {
		t.Errorf("got INFORMATION_SCHEMA.TABLES row %q", got)
	}

	// tables.list reports each table's type.
	types := map[string]bigquery.TableType{}
	for tables := dataset.Tables(ctx); ; {
		table, err := tables.Next()
		if err == iterator.Done {
			break
		} else if err != nil {
			t.Fatal(err)
		}
		types[table.TableID] = metadata(table.TableID).Type
	}
	if types["source"] != bigquery.RegularTable || types["v"] != bigquery.ViewTable {
		t.Errorf("got table types %v", types)
	}

	// A dry run validates the statement without creating the view.
	dryRun := client.Query("CREATE VIEW go_view_statements.dry AS SELECT 1 AS x")
	dryRun.DryRun = true
	if _, err := dryRun.Run(ctx); err != nil {
		t.Fatal(err)
	}
	if _, err := dataset.Table("dry").Metadata(ctx); err == nil {
		t.Error("a dry run created the view")
	}

	destination := client.Query("SELECT 99 AS id")
	destination.Dst = dataset.Table("v")
	destination.WriteDisposition = bigquery.WriteTruncate
	wantError("query to a view", wait(destination.Run(ctx)), "Cannot write to a view")
	copier := dataset.Table("copy").CopierFrom(dataset.Table("v"))
	wantError("copy from a view", wait(copier.Run(ctx)), "Cannot copy a view")
	var row []bigquery.Value
	if err := dataset.Table("v").Read(ctx).Next(&row); err == nil {
		t.Error("tabledata.list read a view")
	}
	_, err := dataset.Table("source").Update(ctx, bigquery.TableMetadataToUpdate{ViewQuery: "SELECT 1 AS id"}, "")
	wantError("tables.patch with a query of a table", err, "is not a view")

	mustRun("DROP VIEW go_view_statements.v")
	if _, err := dataset.Table("v").Metadata(ctx); err == nil {
		t.Error("DROP VIEW left the view")
	}
}
