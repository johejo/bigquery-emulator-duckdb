package storage

import (
	"bytes"
	"fmt"
	"slices"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"github.com/apache/arrow-go/v18/arrow/array"
	"github.com/apache/arrow-go/v18/arrow/ipc"
	"github.com/pierrec/lz4/v4"
)

// The Storage v1 proto specifies raw LZ4, not an LZ4 frame, for response compression.
// uncompressed_byte_size <= 0 means that the payload is not compressed.
func readBlockData(t *testing.T, block *storagepb.ReadRowsResponse) []byte {
	t.Helper()
	data := block.GetArrowRecordBatch().GetSerializedRecordBatch()
	if block.GetAvroRows() != nil {
		data = block.GetAvroRows().GetSerializedBinaryRows()
	}
	if block.GetUncompressedByteSize() > 0 {
		out := make([]byte, block.GetUncompressedByteSize())
		n, err := lz4.UncompressBlock(data, out)
		if err != nil || int64(n) != block.GetUncompressedByteSize() {
			t.Fatalf("LZ4: decoded %d bytes, want %d: %v", n, block.GetUncompressedByteSize(), err)
		}
		return out
	}
	return data
}

// Expectations follow the inserted rows and the compression contracts in the Storage v1 protos.
func TestReadCompression(t *testing.T) {
	t.Parallel()
	rest, read, _ := clients(t)
	path := table(t, rest, "storage_read_compression", "id INT64, s STRING, rec STRUCT<a STRING>, items ARRAY<INT64>")
	query(t, rest, `INSERT INTO storage_read_compression.rows SELECT i, IF(MOD(i, 2) = 0, NULL, REPEAT('x', 1000)), STRUCT('nested' AS a), [i, i+1] FROM UNNEST(GENERATE_ARRAY(1, 2051)) AS i`)
	for _, format := range []storagepb.DataFormat{storagepb.DataFormat_ARROW, storagepb.DataFormat_AVRO} {
		for _, codec := range []string{"response_lz4", "lz4_frame", "zstd"} {
			if format == storagepb.DataFormat_AVRO && codec != "response_lz4" {
				continue
			}
			t.Run(fmt.Sprintf("%s/%s", format, codec), func(t *testing.T) {
				opts := &storagepb.ReadSession_TableReadOptions{}
				if codec == "response_lz4" {
					opts.ResponseCompressionCodec = storagepb.ReadSession_TableReadOptions_RESPONSE_COMPRESSION_CODEC_LZ4.Enum()
				} else {
					c := storagepb.ArrowSerializationOptions_LZ4_FRAME
					if codec == "zstd" {
						c = storagepb.ArrowSerializationOptions_ZSTD
					}
					opts.OutputFormatSerializationOptions = &storagepb.ReadSession_TableReadOptions_ArrowSerializationOptions{ArrowSerializationOptions: &storagepb.ArrowSerializationOptions{BufferCompression: c}}
				}
				s := session(t, read, path, format, opts, 1)
				for _, offset := range []int64{0, 1025, 2051} {
					ids := readIDs(t, read, s, s.Streams[0].Name, offset)
					want := make([]int64, 0, 2051-offset)
					for i := offset + 1; i <= 2051; i++ {
						want = append(want, i)
					}
					if !slices.Equal(ids, want) {
						t.Fatalf("offset %d: decoded %d IDs, want %d", offset, len(ids), len(want))
					}
				}
				for _, block := range responses(t, read, s.Streams[0].Name, 0) {
					if codec == "response_lz4" && block.GetUncompressedByteSize() <= 0 {
						t.Fatal("compressible response was not compressed")
					}
					if codec != "response_lz4" {
						if block.UncompressedByteSize != nil {
							t.Fatal("Arrow compression must not set response compression metadata")
						}
						if int64(len(block.GetArrowRecordBatch().GetSerializedRecordBatch())) >= block.RowCount*500 {
							t.Fatal("Arrow buffers were not compressed")
						}
					}
					if format != storagepb.DataFormat_ARROW {
						continue
					}
					data := append(bytes.Clone(s.GetArrowSchema().GetSerializedSchema()), readBlockData(t, block)...)
					reader, err := ipc.NewReader(bytes.NewReader(data))
					if err != nil {
						t.Fatal(err)
					}
					for reader.Next() {
						r := reader.RecordBatch()
						ids := r.Column(0).(*array.Int64)
						text := r.Column(1).(*array.String)
						nested := r.Column(2).(*array.Struct).Field(0).(*array.String)
						list := r.Column(3).(*array.List)
						values := list.ListValues().(*array.Int64)
						for i := 0; i < ids.Len(); i++ {
							id := ids.Value(i)
							if text.IsNull(i) != (id%2 == 0) || (id%2 != 0 && text.Value(i) != strings.Repeat("x", 1000)) || nested.Value(i) != "nested" {
								t.Fatalf("invalid string buffers at id %d", id)
							}
							start, end := list.ValueOffsets(i)
							if end-start != 2 || values.Value(int(start)) != id || values.Value(int(start+1)) != id+1 {
								t.Fatalf("invalid list buffers at id %d", id)
							}
						}
					}
					err = reader.Err()
					reader.Release()
					if err != nil {
						t.Fatal(err)
					}
				}
			})
		}
	}
}

func TestReadResponseCompressionFallback(t *testing.T) {
	t.Parallel()
	rest, read, _ := clients(t)
	path := table(t, rest, "storage_read_compression_fallback", "id INT64")
	opts := &storagepb.ReadSession_TableReadOptions{ResponseCompressionCodec: storagepb.ReadSession_TableReadOptions_RESPONSE_COMPRESSION_CODEC_LZ4.Enum()}
	// An empty Avro payload and a single small row cannot benefit from LZ4 compression.
	for _, populated := range []bool{false, true} {
		if populated {
			query(t, rest, "INSERT INTO storage_read_compression_fallback.rows VALUES (1)")
		}
		s := session(t, read, path, storagepb.DataFormat_AVRO, opts, 1)
		blocks := responses(t, read, s.Streams[0].Name, 0)
		if len(blocks) != 1 || blocks[0].GetUncompressedByteSize() != -1 {
			t.Fatalf("expected uncompressed fallback: %v", blocks)
		}
		ids := readIDs(t, read, s, s.Streams[0].Name, 0)
		if populated && !slices.Equal(ids, []int64{1}) || !populated && len(ids) != 0 {
			t.Fatal(ids)
		}
	}
}
