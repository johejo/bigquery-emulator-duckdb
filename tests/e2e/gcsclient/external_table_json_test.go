package gcsclient

import (
	"fmt"
	"reflect"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// Native JSON scalars, base64 BYTES and nested/repeated fields follow
// third_party/bigquery_docs/loading-data-cloud-storage-json.md. The format alias follows
// third_party/bigquery_docs/reference/standard-sql/data-definition-language.md.
func TestExternalJSON(t *testing.T) {
	configure(t)
	objects(t, map[string][]byte{
		"a.json": []byte("{\"id\":\"9007199254740993\",\"text\":\"2020-01-01T00:00:00Z\",\"ok\":true,\"score\":1.25,\"data\":\"YWJj\",\"nested\":{\"x\":7},\"items\":[{\"s\":\"a\"}]}\n"),
		"b.json": []byte("{\"id\":2,\"text\":\"001\",\"ok\":false,\"score\":2.5,\"data\":\"\",\"nested\":{\"x\":8},\"items\":[]}\n"),
	})
	client := start(t)
	for _, format := range []string{"JSON", "NEWLINE_DELIMITED_JSON", "api"} {
		name := fmt.Sprintf("json_%s", format)
		table := client.Dataset("gcs").Table(name)
		if format == "api" {
			if err := table.Create(t.Context(), &bigquery.TableMetadata{
				Schema: bigquery.Schema{
					{Name: "id", Type: bigquery.IntegerFieldType},
					{Name: "text", Type: bigquery.StringFieldType},
					{Name: "ok", Type: bigquery.BooleanFieldType},
					{Name: "score", Type: bigquery.FloatFieldType},
					{Name: "data", Type: bigquery.BytesFieldType},
					{Name: "nested", Type: bigquery.RecordFieldType, Schema: bigquery.Schema{{Name: "x", Type: bigquery.IntegerFieldType}}},
					{Name: "items", Type: bigquery.RecordFieldType, Repeated: true, Schema: bigquery.Schema{{Name: "s", Type: bigquery.StringFieldType}}},
				},
				ExternalDataConfig: &bigquery.ExternalDataConfig{
					SourceFormat: bigquery.JSON, SourceURIs: []string{"gs://bucket/*.json"},
				},
			}); err != nil {
				t.Fatal(err)
			}
		} else if err := externalDDL(t.Context(), client, "CREATE EXTERNAL TABLE gcs."+name+`
(id INT64, text STRING, ok BOOL, score FLOAT64, data BYTES, nested STRUCT<x INT64>, items ARRAY<STRUCT<s STRING>>)
OPTIONS (format='`+format+`', uris=['gs://bucket/*.json'])`); err != nil {
			t.Fatal(err)
		}
		metadata, err := table.Metadata(t.Context())
		if err != nil {
			t.Fatal(err)
		}
		if metadata.Type != bigquery.ExternalTable || metadata.ExternalDataConfig == nil || metadata.ExternalDataConfig.SourceFormat != bigquery.JSON {
			t.Fatalf("metadata: %+v", metadata)
		}
		it, err := client.Query("SELECT id, text, ok, score, data, nested.x, ARRAY_LENGTH(items), _FILE_NAME FROM gcs." + name + " ORDER BY id").Read(t.Context())
		if err != nil {
			t.Fatal(err)
		}
		var got [][]bigquery.Value
		for {
			var row []bigquery.Value
			if err := it.Next(&row); err == iterator.Done {
				break
			} else if err != nil {
				t.Fatal(err)
			}
			got = append(got, row)
		}
		want := [][]bigquery.Value{
			{int64(2), "001", false, 2.5, []byte{}, int64(8), int64(0), "gs://bucket/b.json"},
			{int64(9007199254740993), "2020-01-01T00:00:00Z", true, 1.25, []byte("abc"), int64(7), int64(1), "gs://bucket/a.json"},
		}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("rows = %#v, want %#v", got, want)
		}
	}
}

func TestExternalJSONRejectsInvalidSources(t *testing.T) {
	configure(t)
	objects(t, map[string][]byte{
		"syntax.json":   []byte("{\"id\":\n"),
		"overflow.json": []byte("{\"id\":9223372036854775808}\n"),
		"extra.json":    []byte("{\"id\":1,\"extra\":2}\n"),
		"object.json":   []byte("[1,2]\n"),
	})
	client := start(t)
	for _, name := range []string{"syntax", "overflow", "extra", "object"} {
		if err := externalDDL(t.Context(), client, "CREATE EXTERNAL TABLE gcs."+name+` (id INT64)
OPTIONS (format='JSON', uris=['gs://bucket/`+name+`.json'])`); err != nil {
			t.Fatal(err)
		}
		if _, err := client.Query("SELECT * FROM gcs." + name).Read(t.Context()); err == nil {
			t.Errorf("accepted %s source", name)
		}
	}
}
