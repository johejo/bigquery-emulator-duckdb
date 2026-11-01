package gcsclient

import (
	"fmt"
	"reflect"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// The CSV delimiter, skip and quote rules follow
// third_party/bigquery_docs/loading-data-cloud-storage-csv.md. The source header is
// deliberately different from the declared names: without autodetect, columns are positional.
func TestExternalCSV(t *testing.T) {
	configure(t)
	objects(t, map[string][]byte{
		"data.csv": []byte("source_id|source_text\n7|\"a|b\"\n8|\"a\"\"b\"\n9|\n"),
		"bad.csv":  []byte("source_id|source_text\n7|\"first\nsecond\"\n"),
	})
	client := start(t)
	for _, api := range []bool{false, true} {
		name := fmt.Sprintf("csv_%t", api)
		table := client.Dataset("gcs").Table(name)
		if api {
			if err := table.Create(t.Context(), &bigquery.TableMetadata{
				Schema: bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}, {Name: "text", Type: bigquery.StringFieldType}},
				ExternalDataConfig: &bigquery.ExternalDataConfig{
					SourceFormat: bigquery.CSV, SourceURIs: []string{"gs://bucket/data.csv"},
					Options: &bigquery.CSVOptions{SkipLeadingRows: 1, FieldDelimiter: "|"},
				},
			}); err != nil {
				t.Fatal(err)
			}
		} else if err := externalDDL(t.Context(), client, "CREATE EXTERNAL TABLE gcs."+name+` (id INT64, text STRING)
   OPTIONS (format='CSV', uris=['gs://bucket/data.csv'], skip_leading_rows=1, field_delimiter='|')`); err != nil {
			t.Fatal(err)
		}
		metadata, err := table.Metadata(t.Context())
		if err != nil {
			t.Fatal(err)
		}
		if metadata.ExternalDataConfig == nil || metadata.ExternalDataConfig.SourceFormat != bigquery.CSV {
			t.Fatalf("metadata: %+v", metadata)
		}
		options, ok := metadata.ExternalDataConfig.Options.(*bigquery.CSVOptions)
		if !ok || options.SkipLeadingRows != 1 || options.FieldDelimiter != "|" {
			t.Fatalf("CSV options: %+v", metadata.ExternalDataConfig.Options)
		}
		it, err := client.Query("SELECT id, text, _FILE_NAME FROM gcs." + name + " ORDER BY id").Read(t.Context())
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
		want := [][]bigquery.Value{{int64(7), "a|b", "gs://bucket/data.csv"}, {int64(8), "a\"b", "gs://bucket/data.csv"}, {int64(9), nil, "gs://bucket/data.csv"}}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("rows = %v, want %v", got, want)
		}
	}
	if err := externalDDL(t.Context(), client, `CREATE EXTERNAL TABLE gcs.bad (id INT64, text STRING)
  OPTIONS (format='CSV', uris=['gs://bucket/bad.csv'], skip_leading_rows=1, field_delimiter='|')`); err != nil {
		t.Fatal(err)
	}
	if _, err := client.Query("SELECT * FROM gcs.bad").Read(t.Context()); err == nil {
		t.Fatal("quoted newline accepted without allow_quoted_newlines")
	}
}
