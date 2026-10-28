package goclient

import (
	"context"
	"math/big"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"github.com/apache/arrow-go/v18/parquet/file"
	"github.com/johejo/bigquery-emulator-duckdb/internal/serverprocess"
	"google.golang.org/api/iterator"
	"google.golang.org/api/option"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/descriptorpb"
)

// These fixtures are ordinary DuckDB numbers. Their decimal text, rather than the emulator's
// output, defines the expected values through each client path.
func TestDuckDBNumericStorage(t *testing.T) {
	t.Parallel()
	binary := os.Getenv("BQ_EMULATOR_BINARY")
	if binary == "" {
		t.Skip("BQ_EMULATOR_BINARY is not set; run just e2e")
	}
	ctx := t.Context()
	dir := t.TempDir()
	cmd := exec.CommandContext(ctx, "duckdb", filepath.Join(dir, "test.duckdb"))
	cmd.Stdin = strings.NewReader(`CREATE SCHEMA numerics;
	  CREATE TABLE numerics.foreign_numbers AS SELECT
	    '9223372036854775808'::UBIGINT AS u, '-12.3456789012'::DECIMAL(20, 10) AS d,
	    42::HUGEINT AS h;`)
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("DuckDB setup: %v: %s", err, output)
	}
	startup, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	p, err := serverprocess.StartEmulator(startup, binary, "", "--host", "127.0.0.1", "--http-port", "0",
		"--data-dir", dir, `--project={"projectId":"test"}`)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := p.Stop(); err != nil {
			t.Error(err)
		}
	})
	client, err := bigquery.NewClient(ctx, "test", option.WithEndpoint(p.URL), option.WithoutAuthentication())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = client.Close() })
	dataset := client.Dataset("numerics")
	source := dataset.Table("foreign_numbers")
	want := []string{"9223372036854775808", "-12.3456789012", "42"}
	check := func(table *bigquery.Table) {
		t.Helper()
		rows := table.Read(ctx)
		var values []bigquery.Value
		if err := rows.Next(&values); err != nil {
			t.Fatal(err)
		}
		if len(values) != len(want) {
			t.Fatalf("values: %v", values)
		}
		for i, text := range want {
			expected, _ := new(big.Rat).SetString(text)
			value, ok := values[i].(*big.Rat)
			if !ok || value.Cmp(expected) != 0 {
				t.Errorf("column %d: got %v, want %s", i, values[i], text)
			}
		}
		if err := rows.Next(&values); err != iterator.Done {
			t.Fatalf("extra row: %v", err)
		}
	}
	check(source)
	// DELETE, TRUNCATE and MERGE must honor the same row-write restriction as INSERT
	// and UPDATE (covered by the restart scenarios), even if they need no conversion.
	for _, sql := range []string{
		"DELETE numerics.foreign_numbers WHERE TRUE",
		"TRUNCATE TABLE numerics.foreign_numbers",
		"MERGE numerics.foreign_numbers t USING (SELECT 1 AS id) s ON TRUE WHEN MATCHED THEN DELETE",
	} {
		job, err := client.Query(sql).Run(ctx)
		if err := runJob(ctx, job, err); err == nil || !strings.Contains(err.Error(), "DML on external numeric storage") {
			t.Fatalf("%s: got %v, want external numeric storage error", sql, err)
		}
	}
	check(source)
	copied := dataset.Table("copied")
	job, err := copied.CopierFrom(source).Run(ctx)
	if err := runJob(ctx, job, err); err != nil {
		t.Fatalf("copy job: %v", err)
	}
	check(copied)

	// Both extract paths must convert to units before formatting or encoding decimal bytes.
	for _, format := range []bigquery.DataFormat{bigquery.JSON, bigquery.Parquet} {
		path := filepath.Join(dir, string(format))
		destination := bigquery.NewGCSReference(path)
		destination.DestinationFormat = format
		job, err := source.ExtractorTo(destination).Run(ctx)
		if err := runJob(ctx, job, err); err != nil {
			t.Fatalf("extract %s: %v", format, err)
		}
		if format == bigquery.JSON {
			data, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			for _, text := range want {
				if !strings.Contains(string(data), `"`+text+`"`) {
					t.Fatalf("JSON extract: %s", data)
				}
			}
			continue
		}
		reader, err := file.OpenParquetFile(path, false)
		if err != nil {
			t.Fatal(err)
		}
		leaves := parquetLeaves{t: t, reader: reader}
		for i, name := range []string{"u", "d", "h"} {
			data := []byte(leaves.values(name)[0].(string))
			units := new(big.Int).SetBytes(data)
			if data[0]&0x80 != 0 {
				units.Sub(units, new(big.Int).Lsh(big.NewInt(1), uint(len(data)*8)))
			}
			if units.Cmp(unscaled(t, want[i], 38)) != 0 {
				t.Errorf("Parquet %s: %s", name, units)
			}
		}
		if err := reader.Close(); err != nil {
			t.Fatal(err)
		}
	}

	// Appending a query, copy or load must not write scaled units into an ordinary integer.
	assertWriteError := func(err error) {
		t.Helper()
		if err == nil || !strings.Contains(err.Error(), "writing to external numeric storage") {
			t.Fatalf("write: got %v, want external numeric storage error", err)
		}
	}
	for _, disposition := range []bigquery.TableWriteDisposition{bigquery.WriteAppend, bigquery.WriteTruncateData} {
		q := client.Query("SELECT * FROM numerics.foreign_numbers")
		q.Dst, q.WriteDisposition = source, disposition
		job, err = q.Run(ctx)
		assertWriteError(runJob(ctx, job, err))
	}
	copier := source.CopierFrom(copied)
	copier.WriteDisposition = bigquery.WriteAppend
	job, err = copier.Run(ctx)
	assertWriteError(runJob(ctx, job, err))
	csv := filepath.Join(dir, "input.csv")
	if err := os.WriteFile(csv, []byte("1,2,3\n"), 0600); err != nil {
		t.Fatal(err)
	}
	loader := source.LoaderFrom(bigquery.NewGCSReference(csv))
	loader.WriteDisposition = bigquery.WriteAppend
	job, err = loader.Run(ctx)
	assertWriteError(runJob(ctx, job, err))

	conn, err := grpc.NewClient(p.GRPC, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	stream, err := storagepb.NewBigQueryWriteClient(conn).AppendRows(ctx)
	if err != nil {
		t.Fatal(err)
	}
	data := protowire.AppendTag(nil, 1, protowire.BytesType)
	data = protowire.AppendBytes(data, []byte{1})
	err = stream.Send(&storagepb.AppendRowsRequest{
		WriteStream: "projects/test/datasets/numerics/tables/foreign_numbers/streams/_default",
		Rows: &storagepb.AppendRowsRequest_ProtoRows{ProtoRows: &storagepb.AppendRowsRequest_ProtoData{
			WriterSchema: &storagepb.ProtoSchema{ProtoDescriptor: &descriptorpb.DescriptorProto{
				Name: proto.String("Row"), Field: []*descriptorpb.FieldDescriptorProto{{
					Name: proto.String("h"), Number: proto.Int32(1),
					Type:  descriptorpb.FieldDescriptorProto_TYPE_BYTES.Enum(),
					Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(),
				}},
			}},
			Rows: &storagepb.ProtoRows{SerializedRows: [][]byte{data}},
		}},
	})
	if err != nil {
		t.Fatal(err)
	}
	response, err := stream.Recv()
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(response.GetError().GetMessage(), "writing to external numeric storage") {
		t.Fatalf("Storage Write: %v", response)
	}
	_ = stream.CloseSend()
	check(source)

	if err := client.EnableStorageReadClient(ctx, option.WithEndpoint(p.GRPC), option.WithoutAuthentication(),
		option.WithGRPCDialOption(grpc.WithTransportCredentials(insecure.NewCredentials()))); err != nil {
		t.Fatal(err)
	}
	check(source)

	// Replacing the physical schema explicitly creates native, writable storage.
	q := client.Query("SELECT * FROM numerics.foreign_numbers")
	q.Dst, q.WriteDisposition = source, bigquery.WriteTruncate
	job, err = q.Run(ctx)
	if err := runJob(ctx, job, err); err != nil {
		t.Fatalf("replace: %v", err)
	}
	job, err = client.Query("UPDATE numerics.foreign_numbers SET h = h + 1 WHERE TRUE").Run(ctx)
	if err := runJob(ctx, job, err); err != nil {
		t.Fatalf("update replaced table: %v", err)
	}
	want[2] = "43"
	check(source)
}
