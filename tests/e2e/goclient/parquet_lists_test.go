package goclient

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"github.com/apache/arrow-go/v18/parquet"
	"github.com/apache/arrow-go/v18/parquet/file"
	"github.com/apache/arrow-go/v18/parquet/schema"
)

// parquetLeaf is the values and levels of one INT64 column chunk.
type parquetLeaf struct {
	values    []int64
	def, reps []int16
}

// writeParquetColumn writes a Parquet file of one row whose only column is `column`, with
// `leaves` in the order of its leaf columns, and returns its path. Unlike pqarrow, it writes any
// shape the format allows.
func writeParquetColumn(t *testing.T, column schema.Node, leaves ...parquetLeaf) string {
	t.Helper()
	root := schema.MustGroup(schema.NewGroupNode("schema", parquet.Repetitions.Required,
		schema.FieldList{column}, -1))
	path := filepath.Join(t.TempDir(), "source.parquet")
	output, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	writer := file.NewParquetWriter(output, root)
	group, err := writer.AppendRowGroupChecked()
	if err != nil {
		t.Fatal(err)
	}
	for _, leaf := range leaves {
		chunk, err := group.NextColumn()
		if err != nil {
			t.Fatal(err)
		}
		if _, err := chunk.(*file.Int64ColumnChunkWriter).WriteBatch(leaf.values, leaf.def, leaf.reps); err != nil {
			t.Fatal(err)
		}
	}
	if err := group.Close(); err != nil {
		t.Fatal(err)
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	return path
}

// describeSchema writes `s` as "name TYPE MODE", with a RECORD's fields in parentheses.
func describeSchema(s bigquery.Schema) string {
	var fields []string
	for _, field := range s {
		mode := "NULLABLE"
		if field.Repeated {
			mode = "REPEATED"
		} else if field.Required {
			mode = "REQUIRED"
		}
		text := fmt.Sprintf("%s %s %s", field.Name, field.Type, mode)
		if len(field.Schema) > 0 {
			text += " (" + describeSchema(field.Schema) + ")"
		}
		fields = append(fields, text)
	}
	return strings.Join(fields, ", ")
}

// A Parquet load reads each shape of list the format allows as BigQuery does, with and without
// list inference, and rejects the shapes the emulator does not read as BigQuery does.
func TestParquetLists(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_parquet_lists")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })

	optional, repeated := parquet.Repetitions.Optional, parquet.Repetitions.Repeated
	list := func(fields ...schema.Node) schema.Node {
		return schema.MustGroup(schema.NewGroupNodeConverted("a", optional, fields,
			schema.ConvertedTypes.List, -1))
	}
	group := func(name string, repetition parquet.Repetition, fields ...schema.Node) schema.Node {
		return schema.MustGroup(schema.NewGroupNode(name, repetition, fields, -1))
	}
	integer := func(name string, repetition parquet.Repetition) schema.Node {
		return schema.NewInt64Node(name, repetition, -1)
	}
	// Two values, the second repeating the first's list.
	pair := func(definition int16, values ...int64) parquetLeaf {
		return parquetLeaf{values, []int16{definition, definition}, []int16{0, 1}}
	}

	// Without list inference, which is the default, BigQuery loads a LIST group as a RECORD of
	// its repeated field: "a RECORD NULLABLE (list RECORD REPEATED (element INTEGER NULLABLE))"
	// for the standard list and "a RECORD NULLABLE (array INTEGER REPEATED)" for a two-level
	// one. The emulator rejects those.
	const unsupported = "The emulator does not support"
	for _, c := range []struct {
		name      string
		file      string
		inference bool
		// The schema and the row BigQuery loads, or the start of the emulator's error.
		schema, row, err string
	}{
		{
			name: "three_level", inference: true,
			file:   writeParquetColumn(t, list(group("list", repeated, integer("element", optional))), pair(3, 1, 2)),
			schema: "a INTEGER REPEATED", row: `{"a":[1,2]}`,
		},
		{
			name: "two_level", inference: true,
			file:   writeParquetColumn(t, list(integer("array", repeated)), pair(2, 1, 2)),
			schema: "a INTEGER REPEATED", row: `{"a":[1,2]}`,
		},
		{
			name: "repeated_group", inference: true,
			file: writeParquetColumn(t, group("a", repeated, integer("x", optional), integer("y", optional)),
				pair(2, 1, 3), pair(2, 2, 4)),
			schema: "a RECORD REPEATED (x INTEGER NULLABLE, y INTEGER NULLABLE)",
			row:    `{"a":[{"x":1,"y":2},{"x":3,"y":4}]}`,
		},
		{
			name: "repeated_group_without_inference",
			file: writeParquetColumn(t, group("a", repeated, integer("x", optional), integer("y", optional)),
				pair(2, 1, 3), pair(2, 2, 4)),
			schema: "a RECORD REPEATED (x INTEGER NULLABLE, y INTEGER NULLABLE)",
			row:    `{"a":[{"x":1,"y":2},{"x":3,"y":4}]}`,
		},
		{
			name: "repeated_leaf", inference: true,
			file:   writeParquetColumn(t, integer("a", repeated), pair(1, 1, 2)),
			schema: "a INTEGER REPEATED", row: `{"a":[1,2]}`,
		},
		{
			name:   "repeated_leaf_without_inference",
			file:   writeParquetColumn(t, integer("a", repeated), pair(1, 1, 2)),
			schema: "a INTEGER REPEATED", row: `{"a":[1,2]}`,
		},
		{
			name: "three_level_without_inference",
			file: writeParquetColumn(t, list(group("list", repeated, integer("element", optional))), pair(3, 1, 2)),
			err:  unsupported,
		},
		{
			name: "two_level_without_inference",
			file: writeParquetColumn(t, list(integer("array", repeated)), pair(2, 1, 2)),
			err:  unsupported,
		},
		// By the format's backward compatibility rules, a repeated group of one field named
		// array, or the list's name with _tuple, is the element, where DuckDB reads a list of the
		// field. For array, BigQuery loads "a RECORD REPEATED (x INTEGER NULLABLE)" and the row
		// {"a":[{"x":1},{"x":2}]}.
		{
			name: "array_group", inference: true,
			file: writeParquetColumn(t, list(group("array", repeated, integer("x", optional))), pair(3, 1, 2)),
			err:  unsupported,
		},
		{
			name: "tuple_group", inference: true,
			file: writeParquetColumn(t, list(group("a_tuple", repeated, integer("x", optional))), pair(3, 1, 2)),
			err:  unsupported,
		},
	} {
		t.Run(c.name, func(t *testing.T) {
			source := bigquery.NewGCSReference(c.file)
			source.SourceFormat = bigquery.Parquet
			if c.inference {
				source.ParquetOptions = &bigquery.ParquetOptions{EnableListInference: true}
			}
			table := dataset.Table(c.name)
			job, err := table.LoaderFrom(source).Run(ctx)
			err = runJob(ctx, job, err)
			if c.err != "" {
				if err == nil || !strings.Contains(err.Error(), c.err) {
					t.Fatalf("load: got %v, want an error containing %q", err, c.err)
				}
				return
			}
			if err != nil {
				t.Fatalf("load: %v", err)
			}
			metadata, err := table.Metadata(ctx)
			if err != nil {
				t.Fatal(err)
			}
			if got := describeSchema(metadata.Schema); got != c.schema {
				t.Errorf("schema = %s, want %s", got, c.schema)
			}
			rows, err := client.Query("SELECT TO_JSON_STRING(t) FROM go_parquet_lists." + c.name + " AS t").Read(ctx)
			if err != nil {
				t.Fatal(err)
			}
			var row []bigquery.Value
			if err := rows.Next(&row); err != nil {
				t.Fatal(err)
			}
			if row[0] != c.row {
				t.Errorf("row = %v, want %s", row[0], c.row)
			}
		})
	}
}
