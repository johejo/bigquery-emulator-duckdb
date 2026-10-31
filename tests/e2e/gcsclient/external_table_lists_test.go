package gcsclient

import (
	"fmt"
	"reflect"
	"testing"

	"cloud.google.com/go/bigquery"
	"github.com/apache/arrow-go/v18/parquet"
	"github.com/apache/arrow-go/v18/parquet/schema"
	"google.golang.org/api/iterator"
)

// LIST inference removes the list and element wrappers and makes the field REPEATED, as
// [Loading Parquet data](third_party/bigquery_docs/loading-data-cloud-storage-parquet.md)
// specifies for persistent external tables as well as loads. Nullable elements are covered by
// the independently recorded external_table_lists.api.yaml scenarios.
func TestExternalParquetListInference(t *testing.T) {
	configure(t)
	optional, required, repeated := parquet.Repetitions.Optional, parquet.Repetitions.Required, parquet.Repetitions.Repeated
	group := func(name string, repetition parquet.Repetition, fields ...schema.Node) schema.Node {
		return schema.MustGroup(schema.NewGroupNode(name, repetition, fields, -1))
	}
	list := func(name string, element schema.Node) schema.Node {
		return schema.MustGroup(schema.NewGroupNodeConverted(name, optional,
			schema.FieldList{group("list", repeated, element)}, schema.ConvertedTypes.List, -1))
	}
	// Three rows: NULL, [], and [8, 9], in both scalar and STRUCT lists.
	objects(t, map[string][]byte{"lists.parquet": parquetColumns(t,
		parquetColumn{node: list("numbers", schema.NewInt64Node("element", required, -1)),
			values: []int64{8, 9}, levels: []int16{0, 1, 2, 2}, repetitions: []int16{0, 0, 0, 1}},
		parquetColumn{node: list("records", group("element", required, schema.NewInt64Node("x", required, -1))),
			values: []int64{8, 9}, levels: []int16{0, 1, 2, 2}, repetitions: []int16{0, 0, 0, 1}},
	)})
	client := start(t)
	// Explicit false retains the wrappers, just as the default does.
	if err := externalDDL(t.Context(), client, `CREATE EXTERNAL TABLE gcs.wrapped
		OPTIONS (format = 'PARQUET', uris = ['gs://bucket/lists.parquet'], enable_list_inference = false)`); err != nil {
		t.Fatal(err)
	}
	wrapped, err := client.Dataset("gcs").Table("wrapped").Metadata(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	if len(wrapped.Schema) != 2 || wrapped.Schema[0].Type != bigquery.RecordFieldType || wrapped.Schema[0].Repeated ||
		len(wrapped.Schema[0].Schema) != 1 || wrapped.Schema[0].Schema[0].Name != "list" ||
		!wrapped.Schema[0].Schema[0].Repeated {
		t.Fatalf("false option unwrapped LIST: %+v", wrapped.Schema)
	}
	for _, api := range []bool{false, true} {
		for _, explicit := range []bool{false, true} {
			name := fmt.Sprintf("lists_%t_%t", api, explicit)
			table := client.Dataset("gcs").Table(name)
			if api {
				metadata := &bigquery.TableMetadata{ExternalDataConfig: &bigquery.ExternalDataConfig{
					SourceFormat: bigquery.Parquet, SourceURIs: []string{"gs://bucket/lists.parquet"},
					Options: &bigquery.ParquetOptions{EnableListInference: true},
				}}
				if explicit {
					metadata.Schema = bigquery.Schema{
						{Name: "numbers", Type: bigquery.IntegerFieldType, Repeated: true},
						{Name: "records", Type: bigquery.RecordFieldType, Repeated: true,
							Schema: bigquery.Schema{{Name: "x", Type: bigquery.IntegerFieldType, Required: true}}},
					}
				}
				if err := table.Create(t.Context(), metadata); err != nil {
					t.Fatal(err)
				}
			} else {
				columns := ""
				if explicit {
					columns = "(numbers ARRAY<INT64>, records ARRAY<STRUCT<x INT64>>)"
				}
				if err := externalDDL(t.Context(), client, "CREATE EXTERNAL TABLE gcs."+name+" "+columns+
					" OPTIONS (format = 'PARQUET', uris = ['gs://bucket/lists.parquet'], enable_list_inference = true)"); err != nil {
					t.Fatal(err)
				}
			}
			metadata, err := table.Metadata(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			if metadata.ExternalDataConfig == nil {
				t.Fatalf("%s: missing external configuration", name)
			}
			options, ok := metadata.ExternalDataConfig.Options.(*bigquery.ParquetOptions)
			if !ok || !options.EnableListInference {
				t.Fatalf("%s: LIST inference option not retained: %+v", name, metadata.ExternalDataConfig.Options)
			}
			if len(metadata.Schema) != 2 || metadata.Schema[0].Type != bigquery.IntegerFieldType ||
				!metadata.Schema[0].Repeated || metadata.Schema[1].Type != bigquery.RecordFieldType ||
				!metadata.Schema[1].Repeated || len(metadata.Schema[1].Schema) != 1 || metadata.Schema[1].Schema[0].Name != "x" {
				t.Fatalf("%s: schema = %+v", name, metadata.Schema)
			}
			it, err := client.Query("SELECT TO_JSON_STRING(t) FROM gcs." + name + " AS t ORDER BY 1").Read(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			var got []string
			for {
				var row []bigquery.Value
				if err := it.Next(&row); err == iterator.Done {
					break
				} else if err != nil {
					t.Fatal(err)
				}
				got = append(got, row[0].(string))
			}
			want := []string{`{"numbers":[8,9],"records":[{"x":8},{"x":9}]}`, `{"numbers":[],"records":[]}`, `{"numbers":[],"records":[]}`}
			if !reflect.DeepEqual(got, want) {
				t.Fatalf("%s: rows = %v, want %v", name, got, want)
			}
		}
	}
}
