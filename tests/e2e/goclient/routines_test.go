package goclient

import (
	"context"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"cloud.google.com/go/civil"
	"google.golang.org/api/iterator"
)

// queryValue runs sql with dataset as its default dataset and returns the single value it
// selects.
func queryValue(t *testing.T, client *bigquery.Client, dataset, sql string) (bigquery.Value, error) {
	t.Helper()
	job, _, err := runScript(t, client, dataset, sql)
	if err != nil {
		return nil, err
	}
	rows, err := job.Read(context.Background())
	if err != nil {
		return nil, err
	}
	var row []bigquery.Value
	if err := rows.Next(&row); err != nil {
		return nil, err
	}
	return row[0], nil
}

// Persistent SQL UDFs that CREATE FUNCTION defines are called by other queries through their
// dataset, as https://cloud.google.com/bigquery/docs/user-defined-functions describes, and
// routines.get and routines.list describe them.
func TestPersistentSqlUdfs(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_routines")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	run := func(sql string) error {
		t.Helper()
		_, _, err := runScript(t, client, "", sql)
		return err
	}
	expect := func(sql string, want bigquery.Value) {
		t.Helper()
		got, err := queryValue(t, client, "", sql)
		if err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
		if got != want {
			t.Errorf("%s: got %v, want %v", sql, got, want)
		}
	}

	_, status, err := runScript(t, client, "",
		`CREATE FUNCTION go_routines.add_one(x INT64) RETURNS INT64 AS (x + 1)
		OPTIONS (description = 'adds one')`)
	if err != nil {
		t.Fatal(err)
	}
	statistics := status.Statistics.Details.(*bigquery.QueryStatistics)
	if statistics.StatementType != "CREATE_FUNCTION" {
		t.Errorf("got statement type %q, want CREATE_FUNCTION", statistics.StatementType)
	}
	if target := statistics.DDLTargetRoutine; target == nil || target.ProjectID != client.Project() ||
		target.DatasetID != "go_routines" || target.RoutineID != "add_one" {
		t.Errorf("got DDL target routine %+v, want go_routines.add_one", target)
	}
	expect("SELECT go_routines.add_one(1)", int64(2))
	expect("SELECT `"+client.Project()+".go_routines.add_one`(41)", int64(42))
	expect("SELECT `"+client.Project()+"`.go_routines.add_one(-1)", int64(0))

	// A body names other UDFs and tables with their datasets.
	for _, sql := range []string{
		"CREATE FUNCTION go_routines.add_two(x INT64) AS (go_routines.add_one(go_routines.add_one(x)))",
		"CREATE TABLE go_routines.t AS SELECT 1 AS id UNION ALL SELECT 2",
		"CREATE FUNCTION go_routines.row_count() AS ((SELECT COUNT(*) FROM go_routines.t))",
		"CREATE FUNCTION go_routines.twice(x ANY TYPE) AS ([x, x])",
	} {
		if err := run(sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	expect("SELECT go_routines.add_two(1)", int64(3))
	expect("SELECT go_routines.row_count()", int64(2))
	expect("SELECT ARRAY_LENGTH(go_routines.twice('a'))", int64(2))
	expect("SELECT go_routines.twice(DATE '2024-01-02')[OFFSET(1)]", civil.Date{Year: 2024, Month: 1, Day: 2})
	// Each call resolves the body anew, so it sees the table as it is.
	if err := run("INSERT go_routines.t (id) VALUES (3)"); err != nil {
		t.Fatal(err)
	}
	expect("SELECT go_routines.row_count()", int64(3))

	// The default dataset completes a name without a project.
	if got, err := queryValue(t, client, "go_routines", "SELECT go_routines.add_one(id) FROM t ORDER BY id LIMIT 1"); err != nil || got != int64(2) {
		t.Errorf("got %v and %v, want 2", got, err)
	}

	// A multi-statement query calls a function it creates.
	expect("CREATE FUNCTION go_routines.square(x FLOAT64) AS (x * x); SELECT go_routines.square(1.5)",
		2.25)

	// OR REPLACE replaces a function, and IF NOT EXISTS keeps it.
	if err := run("CREATE OR REPLACE FUNCTION go_routines.add_one(x INT64) AS (x + 100)"); err != nil {
		t.Fatal(err)
	}
	if err := run("CREATE FUNCTION IF NOT EXISTS go_routines.add_one(x INT64) AS (x)"); err != nil {
		t.Fatal(err)
	}
	expect("SELECT go_routines.add_one(1)", int64(101))
	expect("SELECT go_routines.add_two(1)", int64(201))
	if err := run("CREATE FUNCTION go_routines.add_one(x INT64) AS (x)"); err == nil {
		t.Error("CREATE FUNCTION over an existing function: got no error")
	}

	// routines.get and routines.list describe them.
	if err := run(`CREATE OR REPLACE FUNCTION go_routines.add_one(x INT64) RETURNS INT64 AS (x + 1)
		OPTIONS (description = 'adds one')`); err != nil {
		t.Fatal(err)
	}
	metadata, err := dataset.Routine("add_one").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if metadata.Type != "SCALAR_FUNCTION" || metadata.Language != "SQL" ||
		metadata.Body != "x + 1" || metadata.Description != "adds one" ||
		metadata.CreationTime.IsZero() || metadata.LastModifiedTime.IsZero() {
		t.Errorf("got %+v", metadata)
	}
	if len(metadata.Arguments) != 1 || metadata.Arguments[0].Name != "x" ||
		metadata.Arguments[0].DataType == nil || metadata.Arguments[0].DataType.TypeKind != "INT64" {
		t.Errorf("got arguments %+v, want x INT64", metadata.Arguments)
	}
	if metadata.ReturnType == nil || metadata.ReturnType.TypeKind != "INT64" {
		t.Errorf("got return type %+v, want INT64", metadata.ReturnType)
	}
	templated, err := dataset.Routine("twice").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if len(templated.Arguments) != 1 || templated.Arguments[0].Kind != "ANY_TYPE" ||
		templated.Arguments[0].DataType != nil {
		t.Errorf("got arguments %+v, want x ANY TYPE", templated.Arguments)
	}
	var names []string
	routines := dataset.Routines(ctx)
	for {
		routine, err := routines.Next()
		if err == iterator.Done {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		names = append(names, routine.RoutineID)
	}
	if want := []string{"add_one", "add_two", "row_count", "square", "twice"}; !reflect.DeepEqual(names, want) {
		t.Errorf("got routines %v, want %v", names, want)
	}

	// A dataset with routines is still in use.
	other := client.Dataset("go_routines_in_use")
	_ = other.DeleteWithContents(ctx)
	t.Cleanup(func() { _ = other.DeleteWithContents(ctx) })
	if err := other.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	if err := run("CREATE FUNCTION go_routines_in_use.f() AS (1)"); err != nil {
		t.Fatal(err)
	}
	if err := other.Delete(ctx); err == nil || !strings.Contains(err.Error(), "still in use") {
		t.Errorf("deleting a dataset with a routine: got %v, want it still in use", err)
	}

	// DROP FUNCTION and routines.delete remove them.
	if err := run("DROP FUNCTION go_routines.square"); err != nil {
		t.Fatal(err)
	}
	if err := run("DROP FUNCTION IF EXISTS go_routines.square"); err != nil {
		t.Fatal(err)
	}
	if err := dataset.Routine("twice").Delete(ctx); err != nil {
		t.Fatal(err)
	}
	for _, sql := range []string{"SELECT go_routines.square(1.0)", "SELECT go_routines.twice(1)"} {
		if _, err := queryValue(t, client, "", sql); err == nil ||
			!strings.Contains(err.Error(), "Function not found") {
			t.Errorf("%s: got %v, want the function not found", sql, err)
		}
	}
	if _, err := dataset.Routine("twice").Metadata(ctx); err == nil || !strings.Contains(err.Error(), "Not found") {
		t.Errorf("routines.get of a deleted routine: got %v", err)
	}
}

// The emulator rejects the forms of persistent UDFs it does not run.
func TestPersistentUdfsRejectUnsupportedForms(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_routines_unsupported")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	if _, _, err := runScript(t, client, "", "CREATE FUNCTION go_routines_unsupported.f(x INT64) AS (x)"); err != nil {
		t.Fatal(err)
	}
	for _, sql := range []string{
		"CREATE FUNCTION go_routines_unsupported.js(x FLOAT64) RETURNS FLOAT64 LANGUAGE js AS 'return x'",
		"CREATE FUNCTION go_routines_unsupported.g(x INT64) OPTIONS (library = ['gs://a/b.js']) AS (x)",
		"CREATE FUNCTION go_routines_unsupported.g(x INT64) DETERMINISTIC AS (x)",
		// A view would keep the body it inlines when the function changes.
		"CREATE VIEW go_routines_unsupported.v AS SELECT go_routines_unsupported.f(1) AS y",
	} {
		_, _, err := runScript(t, client, "", sql)
		if err == nil || !strings.Contains(err.Error(), "The emulator does not support") {
			t.Errorf("%s: got %v, want an unsupported error", sql, err)
		}
	}
	if _, _, err := runScript(t, client, "", "CREATE FUNCTION go_routines_missing.f() AS (1)"); err == nil ||
		!strings.Contains(err.Error(), "Not found: Dataset") {
		t.Errorf("a function in a missing dataset: got %v, want the dataset not found", err)
	}
	// A persistent body cannot call a temporary function, which BigQuery documents too.
	if _, _, err := runScript(t, client, "",
		"CREATE TEMP FUNCTION t(x INT64) AS (x); CREATE FUNCTION go_routines_unsupported.g(x INT64) AS (t(x))"); err == nil {
		t.Error("a persistent body calling a temporary function: got no error")
	}
	// Nor can a body call itself, as a replacement that names the function it replaces would.
	if _, _, err := runScript(t, client, "",
		"CREATE OR REPLACE FUNCTION go_routines_unsupported.f(x INT64) AS (go_routines_unsupported.f(x))"); err != nil {
		t.Fatal(err)
	}
	if _, err := queryValue(t, client, "", "SELECT go_routines_unsupported.f(1)"); err == nil {
		t.Error("a recursive call: got no error")
	}
}

// API fields and replacement semantics follow the Routine resource and routines.update
// documentation: https://cloud.google.com/bigquery/docs/reference/rest/v2/routines.
func TestRoutineInsertUpdate(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_routine_api")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	routine := dataset.Routine("f")
	args := []*bigquery.RoutineArgument{{Name: "x", DataType: &bigquery.StandardSQLDataType{TypeKind: "INT64"}}}
	initial := &bigquery.RoutineMetadata{Type: "SCALAR_FUNCTION", Arguments: args,
		ReturnType: &bigquery.StandardSQLDataType{TypeKind: "INT64"}, Body: "x + 1 -- adds one", Description: "adds one"}
	if err := routine.Create(ctx, initial); err != nil {
		t.Fatal(err)
	}
	before, err := routine.Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if before.Language != "SQL" || before.Body != initial.Body || before.Description != initial.Description ||
		before.CreationTime.IsZero() || before.LastModifiedTime.IsZero() || before.ReturnType.TypeKind != "INT64" {
		t.Errorf("unexpected metadata: %+v", before)
	}
	expect := func(sql string, want bigquery.Value) {
		t.Helper()
		got, err := queryValue(t, client, "", sql)
		if err != nil || got != want {
			t.Fatalf("%s: got %v, %v; want %v", sql, got, err, want)
		}
	}
	expect("SELECT go_routine_api.f(41)", int64(42))
	expect("SELECT go_routine_api.f(NULL)", nil)
	if err := routine.Create(ctx, initial); err == nil {
		t.Error("duplicate insert succeeded")
	}
	after, err := routine.Update(ctx, &bigquery.RoutineMetadataToUpdate{
		Type: "SCALAR_FUNCTION", Arguments: args, Body: "x + 2"}, "")
	if err != nil {
		t.Fatal(err)
	}
	if after.Description != "" || after.ReturnType != nil || !after.CreationTime.Equal(before.CreationTime) ||
		after.LastModifiedTime.Before(before.LastModifiedTime) || after.Body != "x + 2" {
		t.Errorf("unexpected replacement metadata: %+v", after)
	}
	expect("SELECT go_routine_api.f(40)", int64(42))
	if _, err := routine.Update(ctx, &bigquery.RoutineMetadataToUpdate{
		Type: "SCALAR_FUNCTION", Arguments: args, Body: "unknown_column"}, ""); err == nil {
		t.Error("invalid replacement succeeded")
	}
	expect("SELECT go_routine_api.f(40)", int64(42))
	if _, err := dataset.Routine("missing").Update(ctx, &bigquery.RoutineMetadataToUpdate{
		Type: "SCALAR_FUNCTION", Body: "1"}, ""); err == nil {
		t.Error("update created a missing routine")
	}
	for _, metadata := range []*bigquery.RoutineMetadata{
		{Type: "SCALAR_FUNCTION", Arguments: []*bigquery.RoutineArgument{{Name: "x", Kind: "ANY_TYPE"}}, Body: "[x, x]"},
		{Type: "SCALAR_FUNCTION", Body: "`" + client.Project() + ".go_routine_api.f`(40)"},
	} {
		r := dataset.Routine("other")
		if err := r.Create(ctx, metadata); err != nil {
			t.Fatal(err)
		}
		if len(metadata.Arguments) != 0 {
			expect("SELECT ARRAY_LENGTH(go_routine_api.other('a'))", int64(2))
		} else {
			expect("SELECT go_routine_api.other()", int64(42))
		}
		if err := r.Delete(ctx); err != nil {
			t.Fatal(err)
		}
	}
	// Routine.definitionBody requires references to other routines to include the project ID.
	if err := dataset.Routine("unqualified").Create(ctx, &bigquery.RoutineMetadata{
		Type: "SCALAR_FUNCTION", Body: "go_routine_api.f(40)"}); err == nil {
		t.Error("unqualified routine reference succeeded")
	}
	if err := dataset.Routine("builtin").Create(ctx, &bigquery.RoutineMetadata{
		Type: "SCALAR_FUNCTION", Body: "NET.HOST('https://example.com')"}); err != nil {
		t.Fatal(err)
	}
	expect("SELECT go_routine_api.builtin()", "example.com")

	composite := dataset.Routine("composite")
	compositeType := &bigquery.StandardSQLDataType{TypeKind: "STRUCT", StructType: &bigquery.StandardSQLStructType{
		Fields: []*bigquery.StandardSQLField{{Name: "values", Type: &bigquery.StandardSQLDataType{
			TypeKind: "ARRAY", ArrayElementType: &bigquery.StandardSQLDataType{TypeKind: "INT64"}}}}}}
	if err := composite.Create(ctx, &bigquery.RoutineMetadata{Type: "SCALAR_FUNCTION",
		Arguments:  []*bigquery.RoutineArgument{{Name: "x", DataType: compositeType}},
		ReturnType: &bigquery.StandardSQLDataType{TypeKind: "FLOAT64"}, Body: "x.values[OFFSET(0)]"}); err != nil {
		t.Fatal(err)
	}
	expect("SELECT go_routine_api.composite(STRUCT([42] AS values))", float64(42))
	compositeMetadata, err := composite.Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(compositeMetadata.Arguments[0].DataType, compositeType) {
		t.Errorf("got composite type %+v, want %+v", compositeMetadata.Arguments[0].DataType, compositeType)
	}

	// DDL and the API use the same persistent definition.
	if _, _, err := runScript(t, client, "", "CREATE OR REPLACE FUNCTION go_routine_api.f(x INT64) AS (x + 3)"); err != nil {
		t.Fatal(err)
	}
	expect("SELECT go_routine_api.f(39)", int64(42))
}
