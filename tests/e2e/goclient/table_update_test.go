package goclient

import (
	"context"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

// Table.Update sends tables.patch, which may add NULLABLE and REPEATED fields after the existing
// ones, records included, relax REQUIRED to NULLABLE and change descriptions, but nothing else.
func TestTableUpdateSchema(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_table_update")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })

	table := dataset.Table("t")
	created := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType, Required: true},
		{Name: "point", Type: bigquery.RecordFieldType, Schema: bigquery.Schema{
			{Name: "x", Type: bigquery.IntegerFieldType},
		}},
		{Name: "items", Type: bigquery.RecordFieldType, Repeated: true, Schema: bigquery.Schema{
			{Name: "sku", Type: bigquery.StringFieldType},
		}},
	}
	if err := table.Create(ctx, &bigquery.TableMetadata{Schema: created}); err != nil {
		t.Fatal(err)
	}
	inserter := table.Inserter()
	if err := inserter.Put(ctx, &bigquery.ValuesSaver{Schema: created, Row: []bigquery.Value{
		int64(1), []bigquery.Value{int64(2)}, []bigquery.Value{[]bigquery.Value{"a"}},
	}}); err != nil {
		t.Fatal(err)
	}

	updated := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType, Description: "the key"},
		{Name: "point", Type: bigquery.RecordFieldType, Schema: bigquery.Schema{
			{Name: "x", Type: bigquery.IntegerFieldType},
			{Name: "y", Type: bigquery.IntegerFieldType},
		}},
		{Name: "items", Type: bigquery.RecordFieldType, Repeated: true, Schema: bigquery.Schema{
			{Name: "sku", Type: bigquery.StringFieldType},
			{Name: "count", Type: bigquery.IntegerFieldType},
		}},
		{Name: "doc", Type: bigquery.JSONFieldType},
		{Name: "tags", Type: bigquery.StringFieldType, Repeated: true},
	}
	metadata, err := table.Update(ctx, bigquery.TableMetadataToUpdate{Schema: updated}, "")
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(metadata.Schema, updated) {
		t.Fatalf("tables.patch: got schema\n%v\nwant\n%v", fields(metadata.Schema), fields(updated))
	}
	if metadata, err = table.Metadata(ctx); err != nil || !reflect.DeepEqual(metadata.Schema, updated) {
		t.Fatalf("tables.get after tables.patch: got %v, %v", metadata, err)
	}

	// Existing rows read the added fields as NULL, or empty for a REPEATED column, and the relaxed
	// column takes NULL.
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
	if err := run("INSERT go_table_update.t (point, doc) VALUES (STRUCT(3, 4), JSON '{}')"); err != nil {
		t.Fatal(err)
	}
	rows, err := client.Query(`SELECT id, point.y, items[SAFE_OFFSET(0)].count, doc, ARRAY_LENGTH(tags)
		FROM go_table_update.t ORDER BY id`).Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range [][]bigquery.Value{{nil, int64(4), nil, "{}", int64(0)}, {int64(1), nil, nil, nil, int64(0)}} {
		var row []bigquery.Value
		if err := rows.Next(&row); err != nil || !reflect.DeepEqual(row, want) {
			t.Fatalf("row = %#v, want %#v; error = %v", row, want, err)
		}
	}

	with := func(change func(bigquery.Schema) bigquery.Schema) bigquery.Schema {
		schema := make(bigquery.Schema, len(updated))
		for i, field := range updated {
			copied := *field
			schema[i] = &copied
		}
		return change(schema)
	}
	for name, test := range map[string]struct {
		schema bigquery.Schema
		error  string
	}{
		"dropped column": {with(func(s bigquery.Schema) bigquery.Schema { return s[:4] }),
			"Field tags is missing in new schema"},
		"dropped record field": {with(func(s bigquery.Schema) bigquery.Schema {
			s[1].Schema = s[1].Schema[:1]
			return s
		}), "Field point.y is missing in new schema"},
		"changed type": {with(func(s bigquery.Schema) bigquery.Schema {
			s[3].Type = bigquery.StringFieldType
			return s
		}), "Field doc has changed type from JSON to STRING"},
		"tightened mode": {with(func(s bigquery.Schema) bigquery.Schema {
			s[0].Required = true
			return s
		}), "Field id has changed mode from NULLABLE to REQUIRED"},
		"added required column": {with(func(s bigquery.Schema) bigquery.Schema {
			return append(s, &bigquery.FieldSchema{Name: "r", Type: bigquery.StringFieldType, Required: true})
		}), "Cannot add required fields"},
		"reordered columns": {with(func(s bigquery.Schema) bigquery.Schema {
			s[3], s[4] = s[4], s[3]
			return s
		}), "Field doc has changed position"},
	} {
		_, err := table.Update(ctx, bigquery.TableMetadataToUpdate{Schema: test.schema}, "")
		if err == nil || !strings.Contains(err.Error(), test.error) {
			t.Errorf("%s: got error %v, want one containing %q", name, err, test.error)
		}
	}
	if metadata, err = table.Metadata(ctx); err != nil || !reflect.DeepEqual(metadata.Schema, updated) {
		t.Fatalf("rejected updates changed the table: got %v, %v", metadata, err)
	}
}

// A view's query can be replaced; its schema follows the new query.
func TestTableUpdateViewQuery(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_view_update")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })

	view := dataset.Table("v")
	if err := view.Create(ctx, &bigquery.TableMetadata{ViewQuery: "SELECT 1 AS a"}); err != nil {
		t.Fatal(err)
	}
	metadata, err := view.Update(ctx, bigquery.TableMetadataToUpdate{ViewQuery: "SELECT 'x' AS b"}, "")
	if err != nil {
		t.Fatal(err)
	}
	want := bigquery.Schema{{Name: "b", Type: bigquery.StringFieldType}}
	if metadata.ViewQuery != "SELECT 'x' AS b" || metadata.UseLegacySQL || !reflect.DeepEqual(metadata.Schema, want) {
		t.Fatalf("got view %q (legacy %v) with schema %v", metadata.ViewQuery, metadata.UseLegacySQL, fields(metadata.Schema))
	}
	if _, err := view.Update(ctx, bigquery.TableMetadataToUpdate{ViewQuery: "SELECT missing"}, ""); err == nil {
		t.Fatal("updating a view to an invalid query succeeded")
	}
	if metadata, err = view.Metadata(ctx); err != nil || metadata.ViewQuery != "SELECT 'x' AS b" {
		t.Fatalf("a failed update changed the view: got %v, %v", metadata, err)
	}
}
