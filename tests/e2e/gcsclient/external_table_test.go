package gcsclient

import (
	"bytes"
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"reflect"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"github.com/apache/arrow-go/v18/parquet"
	"github.com/apache/arrow-go/v18/parquet/file"
	"github.com/apache/arrow-go/v18/parquet/schema"
	"github.com/johejo/bigquery-emulator-duckdb/internal/serverprocess"
	"google.golang.org/api/iterator"
	"google.golang.org/api/option"
)

func externalParquet(t *testing.T, values ...int64) []byte {
	t.Helper()
	column := schema.MustPrimitive(schema.NewPrimitiveNode("id", parquet.Repetitions.Required,
		parquet.Types.Int64, -1, -1))
	return externalParquetFile(t, schema.FieldList{column}, func(group file.SerialRowGroupWriter) {
		chunk, err := group.NextColumn()
		if err != nil {
			t.Fatal(err)
		}
		if _, err := chunk.(*file.Int64ColumnChunkWriter).WriteBatch(values, nil, nil); err != nil {
			t.Fatal(err)
		}
	})
}

func externalParquetFile(t *testing.T, fields schema.FieldList, write func(file.SerialRowGroupWriter)) []byte {
	t.Helper()
	root := schema.MustGroup(schema.NewGroupNode("schema", parquet.Repetitions.Required, fields, -1))
	var output bytes.Buffer
	writer := file.NewParquetWriter(&output, root)
	group, err := writer.AppendRowGroupChecked()
	if err != nil {
		t.Fatal(err)
	}
	write(group)
	if err := group.Close(); err != nil {
		t.Fatal(err)
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	return output.Bytes()
}

func externalRows(t *testing.T, client *bigquery.Client, query string, want []int64) {
	t.Helper()
	q := client.Query(query)
	q.DisableQueryCache = true
	it, err := q.Read(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	var got []int64
	for {
		var row []bigquery.Value
		if err := it.Next(&row); err == iterator.Done {
			break
		} else if err != nil {
			t.Fatal(err)
		}
		got = append(got, row[0].(int64))
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("rows = %v, want %v", got, want)
	}
}

// External queries read the source, not a copy made when the table was created.
// The controlled Storage endpoint makes changes and download failures deterministic.
func TestExternalParquet(t *testing.T) {
	configure(t)
	var contents atomic.Value
	contents.Store(externalParquet(t, 1, 2))
	var missing atomic.Bool
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/storage/v1/b/bucket/o/data.parquet" || missing.Load() {
			http.NotFound(w, r)
			return
		}
		_, _ = w.Write(contents.Load().([]byte))
	}))
	defer server.Close()
	t.Setenv("STORAGE_EMULATOR_HOST", server.URL)
	client := start(t)
	ctx := t.Context()
	table := client.Dataset("gcs").Table("external")
	if err := table.Create(ctx, &bigquery.TableMetadata{
		Schema: bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}},
		ExternalDataConfig: &bigquery.ExternalDataConfig{
			SourceFormat: bigquery.Parquet,
			SourceURIs:   []string{"gs://bucket/data.parquet"},
		},
	}); err != nil {
		t.Fatal(err)
	}
	metadata, err := table.Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if metadata.Type != bigquery.ExternalTable || metadata.ExternalDataConfig == nil ||
		!reflect.DeepEqual(metadata.ExternalDataConfig.SourceURIs, []string{"gs://bucket/data.parquet"}) {
		t.Fatalf("external table metadata = %+v", metadata)
	}
	externalRows(t, client, "SELECT id FROM gcs.external ORDER BY id", []int64{1, 2})
	contents.Store(externalParquet(t, 3))
	externalRows(t, client, "SELECT a.id + b.id FROM gcs.external a CROSS JOIN gcs.external b", []int64{6})
	// A previously successful read must not hide a later source failure behind cached rows.
	missing.Store(true)
	if _, err := client.Query("SELECT * FROM gcs.external").Read(ctx); err == nil {
		t.Fatal("query of missing source succeeded")
	}
	missing.Store(false)
	for _, sql := range []string{
		"INSERT INTO gcs.external VALUES (4)",
		"UPDATE gcs.external SET id = 4 WHERE TRUE",
		"DELETE FROM gcs.external WHERE TRUE",
		"TRUNCATE TABLE gcs.external",
	} {
		if _, err := client.Query(sql).Read(ctx); err == nil {
			t.Fatalf("write succeeded: %s", sql)
		}
	}
	if err := table.Inserter().Put(ctx, []struct {
		ID int64 `bigquery:"id"`
	}{{4}}); err == nil {
		t.Fatal("streaming insert into external table succeeded")
	}
	q := client.Query("SELECT 4 AS id")
	q.Dst, q.WriteDisposition = table, bigquery.WriteTruncate
	if err := externalQuery(ctx, q); err == nil {
		t.Fatal("query destination replaced external table")
	}
	input := bigquery.NewReaderSource(strings.NewReader("{\"id\":4}\n"))
	input.SourceFormat = bigquery.JSON
	loader := table.LoaderFrom(input)
	loader.WriteDisposition = bigquery.WriteTruncate
	job, err := loader.Run(ctx)
	if err == nil {
		status, waitErr := job.Wait(ctx)
		if waitErr == nil && status.Err() == nil {
			t.Fatal("load replaced external table")
		}
	}
	externalRows(t, client, "SELECT id FROM gcs.external", []int64{3})
}

func TestExternalParquetScalars(t *testing.T) {
	configure(t)
	fields := schema.FieldList{
		schema.NewInt64Node("id", parquet.Repetitions.Required, -1),
		schema.NewBooleanNode("flag", parquet.Repetitions.Optional, -1),
		schema.NewFloat64Node("value", parquet.Repetitions.Optional, -1),
		schema.MustPrimitive(schema.NewPrimitiveNodeLogical("text", parquet.Repetitions.Optional,
			schema.StringLogicalType{}, parquet.Types.ByteArray, -1, -1)),
		schema.NewByteArrayNode("data", parquet.Repetitions.Optional, -1),
	}
	contents := externalParquetFile(t, fields, func(group file.SerialRowGroupWriter) {
		for range fields {
			chunk, err := group.NextColumn()
			if err != nil {
				t.Fatal(err)
			}
			levels := []int16{1, 1, 0}
			switch column := chunk.(type) {
			case *file.Int64ColumnChunkWriter:
				_, err = column.WriteBatch([]int64{1, 2, 3}, nil, nil)
			case *file.BooleanColumnChunkWriter:
				_, err = column.WriteBatch([]bool{true, false}, levels, nil)
			case *file.Float64ColumnChunkWriter:
				_, err = column.WriteBatch([]float64{1.25, 0}, levels, nil)
			case *file.ByteArrayColumnChunkWriter:
				values := []parquet.ByteArray{[]byte("日本語"), {}}
				if column.Descr().Name() == "data" {
					values[0] = []byte{0, 255}
				}
				_, err = column.WriteBatch(values, levels, nil)
			}
			if err != nil {
				t.Fatal(err)
			}
		}
	})
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write(contents)
	}))
	defer server.Close()
	t.Setenv("STORAGE_EMULATOR_HOST", server.URL)
	client := start(t)
	if err := externalDDL(t.Context(), client, `CREATE EXTERNAL TABLE gcs.external
		(id INT64, flag BOOL, value FLOAT64, text STRING, data BYTES)
		OPTIONS (format = 'PARQUET', uris = ['gs://bucket/scalars.parquet'])`); err != nil {
		t.Fatal(err)
	}
	it, err := client.Query("SELECT * FROM gcs.external ORDER BY id").Read(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	want := [][]bigquery.Value{
		{int64(1), true, 1.25, "日本語", []byte{0, 255}},
		{int64(2), false, float64(0), "", []byte{}},
		{int64(3), nil, nil, nil, nil},
	}
	for _, expected := range want {
		var row []bigquery.Value
		if err := it.Next(&row); err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(row, expected) {
			t.Errorf("row = %#v, want %#v", row, expected)
		}
	}
	var extra []bigquery.Value
	if err := it.Next(&extra); err != iterator.Done {
		t.Fatalf("extra row = %v, error = %v", extra, err)
	}
}

func TestExternalParquetRestart(t *testing.T) {
	configure(t)
	binary := os.Getenv("BQ_EMULATOR_BINARY")
	if binary == "" {
		t.Skip("BQ_EMULATOR_BINARY is not set")
	}
	contents := externalParquet(t, 7)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write(contents)
	}))
	defer server.Close()
	t.Setenv("STORAGE_EMULATOR_HOST", server.URL)
	dataDir := t.TempDir()
	start := func() (*bigquery.Client, *serverprocess.Emulator) {
		t.Helper()
		ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
		defer cancel()
		process, err := serverprocess.StartEmulator(ctx, binary, "", "--host=127.0.0.1",
			"--http-port=0", `--project={"projectId":"test"}`, "--data-dir="+dataDir)
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = process.Stop() })
		client, err := bigquery.NewClient(t.Context(), "test", option.WithEndpoint(process.URL+"/bigquery/v2/"), option.WithoutAuthentication())
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = client.Close() })
		return client, process
	}
	client, process := start()
	if err := client.Dataset("gcs").Create(t.Context(), nil); err != nil {
		t.Fatal(err)
	}
	if err := externalDDL(t.Context(), client, `CREATE EXTERNAL TABLE gcs.external (id INT64)
		OPTIONS (format = 'PARQUET', uris = ['gs://bucket/data.parquet'], description = 'persisted source')`); err != nil {
		t.Fatal(err)
	}
	if err := process.Stop(); err != nil {
		t.Fatal(err)
	}
	client, _ = start()
	metadata, err := client.Dataset("gcs").Table("external").Metadata(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	if metadata.Type != bigquery.ExternalTable || metadata.Description != "persisted source" {
		t.Fatalf("metadata after restart = %+v", metadata)
	}
	externalRows(t, client, "SELECT id FROM gcs.external", []int64{7})
}

func externalDDL(ctx context.Context, client *bigquery.Client, sql string) error {
	return externalQuery(ctx, client.Query(sql))
}

func externalQuery(ctx context.Context, query *bigquery.Query) error {
	job, err := query.Run(ctx)
	if err != nil {
		return err
	}
	status, err := job.Wait(ctx)
	if err != nil {
		return err
	}
	return status.Err()
}

func TestExternalParquetWildcard(t *testing.T) {
	configure(t)
	one, two := externalParquet(t, 1), externalParquet(t, 2)
	var added atomic.Bool
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/storage/v1/b/bucket/o":
			w.Header().Set("Content-Type", "application/json")
			if added.Load() {
				fmt.Fprint(w, `{"items":[{"name":"data1.parquet"},{"name":"data2.parquet"}]}`)
			} else {
				fmt.Fprint(w, `{"items":[{"name":"data1.parquet"}]}`)
			}
		case "/storage/v1/b/bucket/o/data1.parquet":
			_, _ = w.Write(one)
		case "/storage/v1/b/bucket/o/data2.parquet":
			_, _ = w.Write(two)
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	t.Setenv("STORAGE_EMULATOR_HOST", server.URL)
	client := start(t)
	ctx := t.Context()
	if err := externalDDL(ctx, client, `CREATE EXTERNAL TABLE gcs.external (id INT64)
		OPTIONS (format = 'PARQUET', uris = ['gs://bucket/data*.parquet'])`); err != nil {
		t.Fatal(err)
	}
	externalRows(t, client, "SELECT id FROM gcs.external ORDER BY id", []int64{1})
	added.Store(true)
	externalRows(t, client, "SELECT id FROM gcs.external ORDER BY id", []int64{1, 2})
	if err := externalDDL(ctx, client, `CREATE EXTERNAL TABLE gcs.multiple (id INT64)
		OPTIONS (format = 'PARQUET', uris = ['gs://bucket/data1.parquet', 'gs://bucket/data2.parquet'])`); err != nil {
		t.Fatal(err)
	}
	externalRows(t, client, "SELECT id FROM gcs.multiple ORDER BY id", []int64{1, 2})
	// A logical definition cannot borrow statement-owned downloads.
	if err := externalDDL(ctx, client, "CREATE VIEW gcs.v AS SELECT * FROM gcs.external"); err == nil || !strings.Contains(err.Error(), "Resource-backed") {
		t.Fatalf("stored resource-backed definition: %v", err)
	}
}
