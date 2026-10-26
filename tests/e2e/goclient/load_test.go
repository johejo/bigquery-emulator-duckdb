package goclient

import (
	"context"
	"math/big"
	"os"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

// people is the schema of the people fixtures in tests/e2e/gcs/load-fixtures.
var people = bigquery.Schema{
	{Name: "id", Type: bigquery.IntegerFieldType},
	{Name: "name", Type: bigquery.StringFieldType},
}

// loadGCS loads uris into table as newline-delimited JSON, with schema unless it is nil.
func loadGCS(ctx context.Context, table *bigquery.Table, schema bigquery.Schema,
	disposition bigquery.TableWriteDisposition, uris ...string) error {
	source := bigquery.NewGCSReference(uris...)
	source.SourceFormat = bigquery.JSON
	source.Schema = schema
	loader := table.LoaderFrom(source)
	loader.WriteDisposition = disposition
	return wait(ctx, loader)
}

// A load reads the objects a URI names, with at most one wildcard, as
// https://cloud.google.com/bigquery/docs/batch-loading-data#load-wildcards describes: a wildcard
// matches across subfolders, and an object name is matched literally, escaped or not. A load
// without a schema appends to the table's own.
func TestGCSLoadWildcards(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_gcs_load")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := datasetQueries{t: t, client: client, dataset: dataset}
	carol, dave := []bigquery.Value{int64(3), "carol"}, []bigquery.Value{int64(4), "dave"}

	for _, c := range []struct {
		name, uri string
		want      [][]bigquery.Value
	}{
		{"object", "gs://load-fixtures/nested/people.jsonl", [][]bigquery.Value{carol, dave}},
		{"wildcard", "gs://load-fixtures/wildcard/*-part.jsonl", [][]bigquery.Value{carol, dave}},
		{"filename", "gs://load-fixtures/wildcard-spec/fed-sample*.jsonl", [][]bigquery.Value{carol}},
		{"prefix", "gs://load-fixtures/wildcard-spec/fed-sample*", [][]bigquery.Value{carol, carol, dave}},
		{"extension", "gs://load-fixtures/wildcard-spec/*.jsonl", [][]bigquery.Value{carol, carol, dave}},
	} {
		if err := loadGCS(ctx, dataset.Table(c.name), people, "", c.uri); err != nil {
			t.Errorf("%s: %v", c.uri, err)
			continue
		}
		if got := p.read("SELECT * FROM " + c.name + " ORDER BY id"); !reflect.DeepEqual(got, c.want) {
			t.Errorf("%s: got %v, want %v", c.uri, got, c.want)
		}
	}

	if err := loadGCS(ctx, dataset.Table("special"), people, "", "gs://load-fixtures/special/a b+%1.jsonl"); err != nil {
		t.Fatal(err)
	}
	if err := loadGCS(ctx, dataset.Table("special"), nil, "", "gs://load-fixtures/special/a b+%*"); err != nil {
		t.Fatal(err)
	}
	eve, frank := []bigquery.Value{int64(5), "eve"}, []bigquery.Value{int64(6), "frank"}
	if got, want := p.read("SELECT * FROM special ORDER BY id"), [][]bigquery.Value{eve, eve, frank}; !reflect.DeepEqual(got, want) {
		t.Errorf("special names: got %v, want %v", got, want)
	}
}

// A load that names no object, or an object it cannot read, fails and leaves the table's rows.
func TestGCSLoadErrors(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_gcs_load_errors")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := datasetQueries{t: t, client: client, dataset: dataset}
	table := dataset.Table("t")
	if err := loadGCS(ctx, table, people, "", "gs://load-fixtures/nested/people.jsonl"); err != nil {
		t.Fatal(err)
	}
	rows := p.read("SELECT * FROM t ORDER BY id")

	for _, c := range []struct{ uri, message string }{
		{"gs://load-fixtures/missing/*.jsonl", "No GCS objects match"},
		{"gs://load-fixtures/*/*.jsonl", "Only one wildcard"},
		{"gs://load-*/nested/people.jsonl", ""},
		{"gs://load-fixtures/missing.jsonl", "Could not read GCS object: gs://load-fixtures/missing.jsonl"},
		{"gs://missing-bucket/*.jsonl", "Could not list GCS objects: gs://missing-bucket/*.jsonl"},
		{"gs://load-fixtures", "Invalid GCS URI: gs://load-fixtures"},
		{"gs://load-fixtures/", "Invalid GCS URI: gs://load-fixtures/"},
		{"gs:///people.jsonl", "Invalid GCS URI: gs:///people.jsonl"},
		{"gs://load-fixtures/wildcard/broken.jsonl", ""},
	} {
		err := loadGCS(ctx, table, nil, bigquery.WriteTruncate, c.uri)
		if err == nil || !strings.Contains(err.Error(), c.message) {
			t.Errorf("%s: got %v, want an error containing %q", c.uri, err, c.message)
		}
	}
	if got := p.read("SELECT * FROM t ORDER BY id"); !reflect.DeepEqual(got, rows) {
		t.Errorf("rows after failed loads = %v, want %v", got, rows)
	}
}

// A gzip-compressed file loads whether it is uploaded or read from GCS.
func TestGzipLoad(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_gzip_load")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := datasetQueries{t: t, client: client, dataset: dataset}
	table := dataset.Table("t")

	file, err := os.Open("../gcs/load-fixtures/compressed/people.csv.gz")
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	upload := bigquery.NewReaderSource(file)
	upload.SourceFormat = bigquery.CSV
	upload.SkipLeadingRows = 1
	upload.Schema = people
	if err := wait(ctx, table.LoaderFrom(upload)); err != nil {
		t.Fatalf("upload: %v", err)
	}
	gcs := bigquery.NewGCSReference("gs://load-fixtures/compressed/people.csv.gz")
	gcs.SourceFormat = bigquery.CSV
	gcs.SkipLeadingRows = 1
	if err := wait(ctx, table.LoaderFrom(gcs)); err != nil {
		t.Fatalf("GCS: %v", err)
	}
	alice, bob := []bigquery.Value{int64(1), "alice"}, []bigquery.Value{int64(2), "bob"}
	if got, want := p.read("SELECT * FROM t ORDER BY id"), [][]bigquery.Value{alice, alice, bob, bob}; !reflect.DeepEqual(got, want) {
		t.Errorf("got %v, want %v", got, want)
	}
}

// CSV and JSON loads keep every digit of a BIGNUMERIC or NUMERIC, which reading the text as a
// DuckDB number or FLOAT64 would lose, at any depth. The values are the bounds BigQuery documents.
func TestBigNumericTextLoads(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_bignumeric_text_load")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := datasetQueries{t: t, client: client, dataset: dataset}
	const maxNumeric = "99999999999999999999999999999.999999999"
	load := func(table string, format bigquery.DataFormat, data string, schema bigquery.Schema) error {
		source := bigquery.NewReaderSource(strings.NewReader(data))
		source.SourceFormat = format
		source.Schema = schema
		return wait(ctx, dataset.Table(table).LoaderFrom(source))
	}
	equal := func(got bigquery.Value, want string) bool {
		r, ok := got.(*big.Rat)
		return ok && r.Cmp(rat(t, want)) == 0
	}

	if err := load("csv", bigquery.CSV, "1,"+maxBigNumeric+"\n2,-1e-38\n3,\n", bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType}, {Name: "n", Type: bigquery.BigNumericFieldType},
	}); err != nil {
		t.Fatalf("CSV: %v", err)
	}
	got := p.read("SELECT n FROM csv ORDER BY id")
	if len(got) != 3 || !equal(got[0][0], maxBigNumeric) || !equal(got[1][0], "-"+tinyBigNumeric) || got[2][0] != nil {
		t.Errorf("CSV: got %v", got)
	}

	schema := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		{Name: "n", Type: bigquery.BigNumericFieldType},
		{Name: "r", Type: bigquery.RecordFieldType, Schema: bigquery.Schema{
			{Name: "ns", Type: bigquery.BigNumericFieldType, Repeated: true},
		}},
		{Name: "m", Type: bigquery.NumericFieldType},
	}
	if err := load("json", bigquery.JSON, `{"id": 1, "n": `+maxBigNumeric+`, "r": {"ns": [`+minBigNumeric+`, 1e-38]}, "m": `+maxNumeric+`}
{"id": 2, "n": "-`+tinyBigNumeric+`", "r": {"ns": []}, "m": -`+maxNumeric+`}
{"id": 3, "n": null, "r": null, "m": null}
`, schema); err != nil {
		t.Fatalf("JSON: %v", err)
	}
	got = p.read("SELECT n, r.ns[SAFE_OFFSET(0)], r.ns[SAFE_OFFSET(1)], ARRAY_LENGTH(r.ns), m FROM json ORDER BY id")
	if len(got) != 3 ||
		!equal(got[0][0], maxBigNumeric) || !equal(got[0][1], minBigNumeric) || !equal(got[0][2], tinyBigNumeric) ||
		!equal(got[0][4], maxNumeric) ||
		!equal(got[1][0], "-"+tinyBigNumeric) || got[1][3] != int64(0) || !equal(got[1][4], "-"+maxNumeric) ||
		!reflect.DeepEqual(got[2], []bigquery.Value{nil, nil, nil, nil, nil}) {
		t.Errorf("JSON: got %v", got)
	}

	// One unit of 10^-38 beyond the largest BIGNUMERIC.
	err := load("overflow", bigquery.JSON,
		`{"n": 578960446186580977117854925043439539266.34992332820282019728792003956564819968}`+"\n",
		bigquery.Schema{{Name: "n", Type: bigquery.BigNumericFieldType}})
	if err == nil || !strings.Contains(err.Error(), "Invalid BIGNUMERIC value") {
		t.Errorf("overflow: got %v, want Invalid BIGNUMERIC value", err)
	}
}
