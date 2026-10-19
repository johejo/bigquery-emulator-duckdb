package goclient

import (
	"context"
	"reflect"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// Table functions share their persistent definitions between SQL DDL, the Routine API and
// INFORMATION_SCHEMA. The API's Routine resource documents TABLE_VALUED_FUNCTION.
func TestTableFunctionLifecycle(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_table_functions")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	args := []*bigquery.RoutineArgument{{Name: "x", DataType: &bigquery.StandardSQLDataType{TypeKind: "INT64"}}}
	routine := dataset.Routine("f")
	if err := routine.Create(ctx, &bigquery.RoutineMetadata{
		Type: "TABLE_VALUED_FUNCTION", Arguments: args, Body: "SELECT x AS y", Description: "table function",
		ReturnTableType: &bigquery.StandardSQLTableType{Columns: []*bigquery.StandardSQLField{
			{Name: "y", Type: &bigquery.StandardSQLDataType{TypeKind: "INT64"}},
		}},
	}); err != nil {
		t.Fatal(err)
	}
	before, err := routine.Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if before.Type != "TABLE_VALUED_FUNCTION" || before.Body != "SELECT x AS y" ||
		before.Description != "table function" || before.ReturnTableType == nil ||
		before.ReturnTableType.Columns[0].Type.TypeKind != "INT64" || !reflect.DeepEqual(before.Arguments[0].DataType, args[0].DataType) {
		t.Fatalf("unexpected routine metadata: %+v", before)
	}
	// A view reads the current definition even when the API replaces the function's body.
	if _, _, err := runScript(t, client, "", "CREATE VIEW go_table_functions.v AS SELECT y FROM go_table_functions.f(7)"); err != nil {
		t.Fatal(err)
	}
	updated, err := routine.Update(ctx, &bigquery.RoutineMetadataToUpdate{
		Type: "TABLE_VALUED_FUNCTION", Arguments: args, Body: "SELECT CAST(x AS STRING) AS y",
	}, "")
	if err != nil {
		t.Fatal(err)
	}
	if !updated.CreationTime.Equal(before.CreationTime) || updated.LastModifiedTime.Before(before.LastModifiedTime) {
		t.Fatalf("updated timestamps = %+v", updated)
	}
	rows, err := client.Query("SELECT y FROM go_table_functions.v").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var row []bigquery.Value
	if err := rows.Next(&row); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(row, []bigquery.Value{"7"}) || rows.Schema[0].Type != bigquery.StringFieldType {
		t.Fatalf("changed view row = %v, schema = %v", row, rows.Schema)
	}
	if _, err := routine.Update(ctx, &bigquery.RoutineMetadataToUpdate{
		Type: "TABLE_VALUED_FUNCTION", Arguments: args, Body: "SELECT missing AS y",
	}, ""); err == nil {
		t.Fatal("invalid replacement succeeded")
	}
	after, err := routine.Metadata(ctx)
	if err != nil || after.Body != updated.Body {
		t.Fatalf("failed replacement changed the body: %+v, %v", after, err)
	}
	_, status, err := runScript(t, client, "", "CREATE OR REPLACE TABLE FUNCTION go_table_functions.f(x INT64) AS (SELECT x AS y)")
	if err != nil {
		t.Fatal(err)
	}
	stats := status.Statistics.Details.(*bigquery.QueryStatistics)
	if stats.StatementType != "CREATE_TABLE_FUNCTION" || stats.DDLTargetRoutine == nil || stats.DDLTargetRoutine.RoutineID != "f" {
		t.Fatalf("unexpected DDL statistics: %+v", stats)
	}
	listed := dataset.Routines(ctx)
	metadata, err := listed.Next()
	if err != nil || metadata.RoutineID != "f" {
		t.Fatalf("list routine = %+v, %v", metadata, err)
	}
	if _, err := listed.Next(); err != iterator.Done {
		t.Fatalf("extra routine: %v", err)
	}
	if _, _, err := runScript(t, client, "", "DROP TABLE FUNCTION go_table_functions.f"); err != nil {
		t.Fatal(err)
	}
	if _, err := routine.Metadata(ctx); err == nil {
		t.Fatal("dropped table function still exists")
	}
}
