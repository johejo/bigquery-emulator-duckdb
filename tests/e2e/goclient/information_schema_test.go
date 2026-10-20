package goclient

import (
	"context"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// INFORMATION_SCHEMA views describe the datasets, tables and columns of a project. The project is
// one of its own, so that the datasets of other tests do not show up in the views.
func TestInformationSchema(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClientForProject(t, "infoschema")
	for _, id := range []string{"a", "b"} {
		dataset := client.Dataset(id)
		_ = dataset.DeleteWithContents(ctx)
		if err := dataset.Create(ctx, nil); err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	}
	for _, sql := range []string{
		"CREATE TABLE a.people (id INT64, name STRING, address STRUCT<city STRING, zip STRING>, tags ARRAY<STRING>)",
		"CREATE TABLE b.events (id INT64 NOT NULL, payload JSON)",
	} {
		if _, _, err := runScript(t, client, "", sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	read := func(sql string) ([][]bigquery.Value, error) {
		rows, err := client.Query(sql).Read(ctx)
		if err != nil {
			return nil, err
		}
		var result [][]bigquery.Value
		for {
			var row []bigquery.Value
			if err := rows.Next(&row); err == iterator.Done {
				return result, nil
			} else if err != nil {
				return nil, err
			}
			result = append(result, row)
		}
	}

	for _, c := range []struct {
		sql  string
		want [][]bigquery.Value
	}{
		{
			"SELECT catalog_name, schema_name, location FROM INFORMATION_SCHEMA.SCHEMATA ORDER BY schema_name",
			[][]bigquery.Value{{"infoschema", "a", "US"}, {"infoschema", "b", "US"}},
		},
		{
			"SELECT schema_name, ddl FROM infoschema.`region-us`.INFORMATION_SCHEMA.SCHEMATA WHERE schema_name = 'a'",
			[][]bigquery.Value{{"a", "CREATE SCHEMA `infoschema.a`\nOPTIONS(\n  location=\"us\"\n);"}},
		},
		{
			"SELECT table_catalog, table_schema, table_name, table_type, ddl FROM a.INFORMATION_SCHEMA.TABLES",
			[][]bigquery.Value{{"infoschema", "a", "people", "BASE TABLE", "CREATE TABLE `infoschema.a.people`\n(\n" +
				"  id INT64,\n  name STRING,\n  address STRUCT<city STRING, zip STRING>,\n  tags ARRAY<STRING>\n);"}},
		},
		{
			"SELECT table_schema, table_name FROM `region-us`.INFORMATION_SCHEMA.TABLES ORDER BY table_schema",
			[][]bigquery.Value{{"a", "people"}, {"b", "events"}},
		},
		{
			"SELECT column_name, ordinal_position, is_nullable, data_type " +
				"FROM `infoschema.a.INFORMATION_SCHEMA.COLUMNS` WHERE table_name = 'people' ORDER BY ordinal_position",
			[][]bigquery.Value{
				{"id", int64(1), "YES", "INT64"},
				{"name", int64(2), "YES", "STRING"},
				{"address", int64(3), "YES", "STRUCT<city STRING, zip STRING>"},
				{"tags", int64(4), "YES", "ARRAY<STRING>"},
			},
		},
		// Columns keep their BigQuery types and modes.
		{
			"SELECT column_name, is_nullable, data_type FROM b.INFORMATION_SCHEMA.COLUMNS ORDER BY ordinal_position",
			[][]bigquery.Value{{"id", "NO", "INT64"}, {"payload", "YES", "JSON"}},
		},
		{
			"SELECT field_path, data_type FROM a.INFORMATION_SCHEMA.COLUMN_FIELD_PATHS " +
				"WHERE column_name = 'address' ORDER BY field_path",
			[][]bigquery.Value{
				{"address", "STRUCT<city STRING, zip STRING>"},
				{"address.city", "STRING"},
				{"address.zip", "STRING"},
			},
		},
		{
			"SELECT is_generated FROM (SELECT * FROM b.INFORMATION_SCHEMA.COLUMNS) WHERE column_name = 'id'",
			[][]bigquery.Value{{"NEVER"}},
		},
	} {
		got, err := read(c.sql)
		if err != nil {
			t.Errorf("%s: %v", c.sql, err)
		} else if !reflect.DeepEqual(got, c.want) {
			t.Errorf("%s: got %q, want %q", c.sql, got, c.want)
		}
	}

	// An empty VIEWS result still exposes all six STRING columns.
	emptyViews, err := client.Query("SELECT * FROM b.INFORMATION_SCHEMA.VIEWS").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var emptyRow []bigquery.Value
	if err := emptyViews.Next(&emptyRow); err != iterator.Done {
		t.Fatalf("empty VIEWS: row = %v, error = %v", emptyRow, err)
	}
	wantNames := []string{"table_catalog", "table_schema", "table_name", "view_definition", "check_option", "use_standard_sql"}
	if len(emptyViews.Schema) != len(wantNames) {
		t.Fatalf("VIEWS schema = %v", emptyViews.Schema)
	}
	for i, field := range emptyViews.Schema {
		if field.Name != wantNames[i] || field.Type != bigquery.StringFieldType || field.Repeated || field.Required {
			t.Errorf("VIEWS column %d = %+v", i, field)
		}
	}

	for sql, want := range map[string]string{
		"SELECT * FROM missing.INFORMATION_SCHEMA.TABLES":    "not found: missing.INFORMATION_SCHEMA.TABLES",
		"SELECT * FROM INFORMATION_SCHEMA.TABLES":            "must be qualified with a dataset",
		"DELETE FROM a.INFORMATION_SCHEMA.TABLES WHERE TRUE": "",
	} {
		if _, err := read(sql); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: got error %v, want %q", sql, err, want)
		}
	}
}
