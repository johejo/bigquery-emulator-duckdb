package goclient

import (
	"bytes"
	"compress/gzip"
	"context"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"github.com/apache/arrow-go/v18/parquet"
	"github.com/apache/arrow-go/v18/parquet/file"
	"github.com/apache/arrow-go/v18/parquet/schema"
)

// extractBucket is a bucket of the fake GCS server that just e2e starts, which extract jobs
// write to.
type extractBucket struct {
	t    *testing.T
	base string
}

func newExtractBucket(t *testing.T, name string) extractBucket {
	t.Helper()
	host := os.Getenv("STORAGE_EMULATOR_HOST")
	if host == "" {
		t.Skip("STORAGE_EMULATOR_HOST is not set; run just e2e")
	}
	base := "http://" + host + "/storage/v1/b"
	response, err := http.Post(base+"?project=test", "application/json",
		strings.NewReader(`{"name": "`+name+`"}`))
	if err != nil {
		t.Fatal(err)
	}
	response.Body.Close()
	if response.StatusCode != http.StatusOK && response.StatusCode != http.StatusConflict {
		t.Fatalf("creating bucket %s: %s", name, response.Status)
	}
	return extractBucket{t: t, base: base + "/" + name}
}

// read returns the content of the object called name.
func (b extractBucket) read(name string) []byte {
	b.t.Helper()
	response, err := http.Get(b.base + "/o/" + url.PathEscape(name) + "?alt=media")
	if err != nil {
		b.t.Fatal(err)
	}
	defer response.Body.Close()
	body, err := io.ReadAll(response.Body)
	if err != nil {
		b.t.Fatal(err)
	}
	if response.StatusCode != http.StatusOK {
		b.t.Fatalf("reading %s: %s: %s", name, response.Status, body)
	}
	return body
}

// parquetLeaves reads each leaf column of a Parquet file of one row group by its dotted path.
type parquetLeaves struct {
	t      *testing.T
	reader *file.Reader
}

func (p parquetLeaves) column(path string) (int, schema.LogicalType) {
	p.t.Helper()
	columns := p.reader.MetaData().Schema
	for i := 0; i < columns.NumColumns(); i++ {
		if columns.Column(i).Path() == path {
			return i, columns.Column(i).LogicalType()
		}
	}
	p.t.Fatalf("no column %s", path)
	return 0, nil
}

// values returns the non-NULL values of the leaf column at path.
func (p parquetLeaves) values(path string) []any {
	p.t.Helper()
	i, _ := p.column(path)
	chunk, err := p.reader.RowGroup(0).Column(i)
	if err != nil {
		p.t.Fatal(err)
	}
	const size = 16
	levels := make([]int16, size)
	var n int
	var values []any
	switch chunk := chunk.(type) {
	case *file.Int32ColumnChunkReader:
		read := make([]int32, size)
		_, n, err = chunk.ReadBatch(size, read, levels, nil)
		for _, v := range read[:n] {
			values = append(values, int64(v))
		}
	case *file.Int64ColumnChunkReader:
		read := make([]int64, size)
		_, n, err = chunk.ReadBatch(size, read, levels, nil)
		for _, v := range read[:n] {
			values = append(values, v)
		}
	case *file.ByteArrayColumnChunkReader:
		read := make([]parquet.ByteArray, size)
		_, n, err = chunk.ReadBatch(size, read, levels, nil)
		for _, v := range read[:n] {
			values = append(values, string(v))
		}
	default:
		p.t.Fatalf("%s: unexpected column type %T", path, chunk)
	}
	if err != nil {
		p.t.Fatal(err)
	}
	return values
}

// duration converts a TIME or TIMESTAMP value in unit to a time.Duration.
func duration(value any, unit schema.TimeUnitType) time.Duration {
	scale := map[schema.TimeUnitType]time.Duration{
		schema.TimeUnitMillis: time.Millisecond, schema.TimeUnitMicros: time.Microsecond, schema.TimeUnitNanos: 1,
	}[unit]
	return time.Duration(value.(int64)) * scale
}

// Extract jobs write CSV, newline-delimited JSON and Parquet in BigQuery's formats, with the
// options a job sets, and count the files they write.
func TestExtract(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	bucket := newExtractBucket(t, "go-extract")
	dataset := client.Dataset("go_extract")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := creationPaths{t: t, client: client, dataset: dataset}
	p.mustRun(`CREATE TABLE scalars AS SELECT
	  1 AS i, 2.5 AS f, NUMERIC '1.5' AS n, TRUE AS b, b'ab' AS bt,
	  TIMESTAMP '2020-01-02 03:04:05.12 UTC' AS ts, DATETIME '2020-01-02 03:04:05.5' AS dt,
	  DATE '2020-01-02' AS d, JSON '{"b": 1, "a": [1, 2]}' AS j,
	  CAST(NULL AS INT64) AS nul, 'x,"y"<&>' AS s`)
	p.mustRun(`CREATE TABLE nested AS SELECT
	  1 AS i, STRUCT(1 AS x, CAST(NULL AS STRING) AS y, TIME '01:02:03' AS t,
	  TIMESTAMP '2020-01-02 03:04:05 UTC' AS ts) AS r,
	  [STRUCT(2 AS x, 'z' AS y)] AS rs, [3, 4] AS ri, CAST(NULL AS STRUCT<a INT64>) AS nr`)
	extract := func(table string, configure func(*bigquery.GCSReference), uris ...string) (*bigquery.Job, error) {
		destination := bigquery.NewGCSReference(uris...)
		if configure != nil {
			configure(destination)
		}
		job, err := dataset.Table(table).ExtractorTo(destination).Run(ctx)
		return job, runJob(ctx, job, err)
	}
	const csvRow = `1|2.5|1.5|true|YWI=|2020-01-02 03:04:05.12 UTC|2020-01-02T03:04:05.5|2020-01-02|"{""a"":[1,2],""b"":1}"||"x,""y""<&>"` + "\n"

	t.Run("csv", func(t *testing.T) {
		job, err := extract("scalars", nil, "gs://go-extract/scalars.csv")
		if err != nil {
			t.Fatal(err)
		}
		want := "i,f,n,b,bt,ts,dt,d,j,nul,s\n" + strings.ReplaceAll(csvRow, "|", ",")
		if got := string(bucket.read("scalars.csv")); got != want {
			t.Errorf("got %q, want %q", got, want)
		}
		statistics, ok := job.LastStatus().Statistics.Details.(*bigquery.ExtractStatistics)
		if !ok || !reflect.DeepEqual(statistics.DestinationURIFileCounts, []int64{1}) {
			t.Errorf("got statistics %+v, want an extract writing one file", job.LastStatus().Statistics.Details)
		}
	})

	t.Run("csv_options", func(t *testing.T) {
		// The Go client sets print_header through ExtractConfig.DisableHeader.
		destination := bigquery.NewGCSReference("gs://go-extract/gzip/part-*.csv.gz")
		destination.Compression = bigquery.Gzip
		destination.FieldDelimiter = "|"
		extractor := dataset.Table("scalars").ExtractorTo(destination)
		extractor.DisableHeader = true
		job, err := extractor.Run(ctx)
		if err := runJob(ctx, job, err); err != nil {
			t.Fatal(err)
		}
		reader, err := gzip.NewReader(bytes.NewReader(bucket.read("gzip/part-000000000000.csv.gz")))
		if err != nil {
			t.Fatal(err)
		}
		got, err := io.ReadAll(reader)
		if err != nil {
			t.Fatal(err)
		}
		if string(got) != csvRow {
			t.Errorf("got %q, want %q", got, csvRow)
		}
	})

	t.Run("json", func(t *testing.T) {
		json := func(d *bigquery.GCSReference) { d.DestinationFormat = bigquery.JSON }
		if _, err := extract("scalars", json, "gs://go-extract/scalars.json"); err != nil {
			t.Fatal(err)
		}
		want := `{"i":"1","f":2.5,"n":"1.5","b":true,"bt":"YWI=","ts":"2020-01-02 03:04:05.12 UTC","dt":"2020-01-02T03:04:05.5","d":"2020-01-02","j":{"a":[1,2],"b":1},"s":"x,\"y\"\u003c\u0026\u003e"}` + "\n"
		if got := string(bucket.read("scalars.json")); got != want {
			t.Errorf("scalars: got %q, want %q", got, want)
		}
		if _, err := extract("nested", json, "gs://go-extract/nested.json"); err != nil {
			t.Fatal(err)
		}
		want = `{"i":"1","r":{"x":"1","t":"01:02:03","ts":"2020-01-02 03:04:05 UTC"},"rs":[{"x":"2","y":"z"}],"ri":["3","4"]}` + "\n"
		if got := string(bucket.read("nested.json")); got != want {
			t.Errorf("nested: got %q, want %q", got, want)
		}
	})

	t.Run("parquet", func(t *testing.T) {
		if _, err := extract("nested", func(d *bigquery.GCSReference) {
			d.DestinationFormat = bigquery.Parquet
			d.Compression = bigquery.Snappy
		}, "gs://go-extract/nested.parquet"); err != nil {
			t.Fatal(err)
		}
		path := filepath.Join(t.TempDir(), "nested.parquet")
		if err := os.WriteFile(path, bucket.read("nested.parquet"), 0o600); err != nil {
			t.Fatal(err)
		}
		reader, err := file.OpenParquetFile(path, false)
		if err != nil {
			t.Fatal(err)
		}
		defer reader.Close()
		leaves := parquetLeaves{t: t, reader: reader}
		if got := leaves.values("r.x"); !reflect.DeepEqual(got, []any{int64(1)}) {
			t.Errorf("r.x = %v, want [1]", got)
		}
		_, logical := leaves.column("r.t")
		timeType, ok := logical.(schema.TimeLogicalType)
		if !ok || !timeType.IsAdjustedToUTC() {
			t.Errorf("r.t is %v, want a TIME adjusted to UTC", logical)
		} else if got := leaves.values("r.t"); len(got) != 1 || duration(got[0], timeType.TimeUnit()) != time.Hour+2*time.Minute+3*time.Second {
			t.Errorf("r.t = %v, want 01:02:03", got)
		}
		_, logical = leaves.column("r.ts")
		timestampType, ok := logical.(schema.TimestampLogicalType)
		want := time.Date(2020, 1, 2, 3, 4, 5, 0, time.UTC).Sub(time.Unix(0, 0).UTC())
		if !ok || timestampType.IsAdjustedToUTC() {
			t.Errorf("r.ts is %v, want a TIMESTAMP not adjusted to UTC", logical)
		} else if got := leaves.values("r.ts"); len(got) != 1 || duration(got[0], timestampType.TimeUnit()) != want {
			t.Errorf("r.ts = %v, want 2020-01-02 03:04:05", got)
		}
		if got := leaves.values("rs.list.element.y"); !reflect.DeepEqual(got, []any{"z"}) {
			t.Errorf("rs[0].y = %v, want [z]", got)
		}
		if got := leaves.values("ri.list.element"); !reflect.DeepEqual(got, []any{int64(3), int64(4)}) {
			t.Errorf("ri = %v, want [3 4]", got)
		}
	})
}

// An extract fails for what BigQuery cannot export: a nested table to CSV, Avro, a view, or a URI
// with a wildcard in its bucket name or more than one wildcard.
func TestExtractErrors(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	newExtractBucket(t, "go-extract-errors")
	dataset := client.Dataset("go_extract_errors")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := creationPaths{t: t, client: client, dataset: dataset}
	p.mustRun("CREATE TABLE scalars AS SELECT 1 AS i")
	p.mustRun("CREATE TABLE nested AS SELECT STRUCT(1 AS x) AS r")
	p.mustRun("CREATE VIEW v AS SELECT 1 AS i")

	for _, c := range []struct {
		table, uri string
		format     bigquery.DataFormat
		message    string
	}{
		{"nested", "gs://go-extract-errors/nested.csv", bigquery.CSV, "nested schema. Field: r"},
		{"scalars", "gs://go-extract-errors/scalars.avro", bigquery.Avro, "Avro extracts"},
		{"scalars", "gs://go-extract-*/scalars.csv", bigquery.CSV, "bucket name"},
		{"scalars", "gs://go-extract-errors/*/part-*.csv", bigquery.CSV, "Only one wildcard"},
		{"v", "gs://go-extract-errors/v.csv", bigquery.CSV, "Cannot extract a view"},
	} {
		destination := bigquery.NewGCSReference(c.uri)
		destination.DestinationFormat = c.format
		job, err := dataset.Table(c.table).ExtractorTo(destination).Run(ctx)
		err = runJob(ctx, job, err)
		if err == nil || !strings.Contains(err.Error(), c.message) {
			t.Errorf("%s to %s: got %v, want an error containing %q", c.table, c.uri, err, c.message)
		}
	}
}
