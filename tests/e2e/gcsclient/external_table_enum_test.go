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

// The enum_as_string option infers ENUM as STRING instead of the default BYTES:
// [CREATE EXTERNAL TABLE](third_party/bigquery_docs/reference/standard-sql/data-definition-language.md#external_table_option_list).
func TestExternalParquetEnumAsString(t *testing.T) {
	configure(t)
	enum := func(name string) schema.Node {
		return logicalNode(t, name, parquet.Repetitions.Optional, schema.EnumLogicalType{}, parquet.Types.ByteArray, -1)
	}
	group := schema.MustGroup(schema.NewGroupNode("nested", parquet.Repetitions.Required, schema.FieldList{enum("value")}, -1))
	values := []parquet.ByteArray{[]byte("青"), []byte("")}
	objects(t, map[string][]byte{"enum.parquet": parquetColumns(t,
		parquetColumn{node: enum("value"), values: values, levels: []int16{1, 1, 0}},
		parquetColumn{node: group, values: values, levels: []int16{1, 1, 0}},
	)})
	client := start(t)
	for _, api := range []bool{false, true} {
		for _, option := range []string{"", "false", "true"} {
			name := fmt.Sprintf("enum_%t_%s", api, option)
			table := client.Dataset("gcs").Table(name)
			asString := option == "true"
			if api {
				config := &bigquery.ExternalDataConfig{SourceFormat: bigquery.Parquet, SourceURIs: []string{"gs://bucket/enum.parquet"}}
				if option != "" {
					config.Options = &bigquery.ParquetOptions{EnumAsString: asString}
				}
				if err := table.Create(t.Context(), &bigquery.TableMetadata{ExternalDataConfig: config}); err != nil {
					t.Fatal(err)
				}
			} else {
				clause := ""
				if option != "" {
					clause = ", enum_as_string = " + option
				}
				if err := externalDDL(t.Context(), client, "CREATE EXTERNAL TABLE gcs."+name+" OPTIONS (format = 'PARQUET', uris = ['gs://bucket/enum.parquet']"+clause+")"); err != nil {
					t.Fatal(err)
				}
			}
			metadata, err := table.Metadata(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			typ := bigquery.BytesFieldType
			projection := "TO_BASE64(value), TO_BASE64(nested.value)"
			expected := "6Z2S"
			if asString {
				typ = bigquery.StringFieldType
				projection = "value, nested.value"
				expected = "青"
			}
			if len(metadata.Schema) != 2 || metadata.Schema[0].Type != typ ||
				len(metadata.Schema[1].Schema) != 1 || metadata.Schema[1].Schema[0].Type != typ {
				t.Fatalf("%s: schema = %+v", name, metadata.Schema)
			}
			if asString {
				options, ok := metadata.ExternalDataConfig.Options.(*bigquery.ParquetOptions)
				if !ok || !options.EnumAsString {
					t.Fatalf("%s: options = %+v", name, metadata.ExternalDataConfig.Options)
				}
			}
			query := "SELECT " + projection + " FROM gcs." + name + " ORDER BY 1 DESC NULLS LAST"
			it, err := client.Query(query).Read(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			var got [][]bigquery.Value
			for {
				var row []bigquery.Value
				err := it.Next(&row)
				if err == iterator.Done {
					break
				}
				if err != nil {
					t.Fatal(err)
				}
				got = append(got, row)
			}
			want := [][]bigquery.Value{{expected, expected}, {"", ""}, {nil, nil}}
			if !reflect.DeepEqual(got, want) {
				t.Fatalf("%s: rows = %v, want %v", name, got, want)
			}
		}
	}
}
