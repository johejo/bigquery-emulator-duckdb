// Command storageprobe calls the Storage v1 RPCs in each form listed here and prints the Markdown
// report docs/storage.md. It checks execution only; client-visible results belong in
// tests/e2e/storage.
package main

import (
	"context"
	_ "embed"
	"flag"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"time"

	"cloud.google.com/go/bigquery"
	"cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"github.com/johejo/bigquery-emulator-duckdb/internal/emulatorprocess"
	"github.com/johejo/bigquery-emulator-duckdb/internal/probereport"
	"google.golang.org/api/option"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/descriptorpb"
	"google.golang.org/protobuf/types/known/timestamppb"
	"google.golang.org/protobuf/types/known/wrapperspb"
)

//go:embed preamble.md
var preamble string

const project = "storage-probe"

// env holds the clients the probes call the emulator with.
type env struct {
	rest   *bigquery.Client
	read   storagepb.BigQueryReadClient
	write  storagepb.BigQueryWriteClient
	tables int
}

// form is a way to call an RPC; call returns the error the emulator reported for it.
type form struct {
	name string
	call func(context.Context, *env) error
}

// rpc is a row of the report.
type rpc struct {
	name, note string
	forms      []form
}

type section struct {
	title string
	rpcs  []rpc
}

// stringSchema is the schema of the tables most probes use, and row is a row of it.
var stringSchema = bigquery.Schema{{Name: "value", Type: bigquery.StringFieldType}}

var row = []byte("\x0a\x05probe")

// descriptor describes row, a message with the STRING field `value`.
var descriptor = &descriptorpb.DescriptorProto{Name: proto.String("Row"), Field: []*descriptorpb.FieldDescriptorProto{{
	Name: proto.String("value"), Number: proto.Int32(1), Type: descriptorpb.FieldDescriptorProto_TYPE_STRING.Enum(),
	Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum()}}}

// table creates a table with the schema and returns its Storage path. Each probe uses tables of
// its own, so that probes do not depend on each other.
func (e *env) table(ctx context.Context, schema bigquery.Schema) (string, error) {
	e.tables++
	id := "t" + strconv.Itoa(e.tables)
	if err := e.rest.Dataset("probe").Table(id).Create(ctx, &bigquery.TableMetadata{Schema: schema}); err != nil {
		return "", err
	}
	return "projects/" + project + "/datasets/probe/tables/" + id, nil
}

// filledTable creates a table of stringSchema holding one row.
func (e *env) filledTable(ctx context.Context) (string, error) {
	path, err := e.table(ctx, stringSchema)
	if err != nil {
		return "", err
	}
	return path, e.append(ctx, path+"/streams/_default", nil)
}

// append sends row to the stream on a connection of its own, at offset if it is not nil.
func (e *env) append(ctx context.Context, stream string, offset *wrapperspb.Int64Value) error {
	return e.send(ctx, &storagepb.AppendRowsRequest{WriteStream: stream, Offset: offset,
		Rows: &storagepb.AppendRowsRequest_ProtoRows{ProtoRows: &storagepb.AppendRowsRequest_ProtoData{
			WriterSchema: &storagepb.ProtoSchema{ProtoDescriptor: descriptor},
			Rows:         &storagepb.ProtoRows{SerializedRows: [][]byte{row}}}}})
}

// send sends the requests on one AppendRows connection and returns the first error reported.
func (e *env) send(ctx context.Context, requests ...*storagepb.AppendRowsRequest) error {
	stream, err := e.write.AppendRows(ctx)
	if err != nil {
		return err
	}
	for _, request := range requests {
		if err := stream.Send(request); err != nil {
			return err
		}
		response, err := stream.Recv()
		if err != nil {
			return err
		}
		if response.GetError() != nil {
			return status.ErrorProto(response.GetError())
		}
	}
	if err := stream.CloseSend(); err != nil {
		return err
	}
	if _, err := stream.Recv(); err != io.EOF {
		return fmt.Errorf("AppendRows did not end: %v", err)
	}
	return nil
}

// stream creates a write stream of the type on a new table.
func (e *env) stream(ctx context.Context, kind storagepb.WriteStream_Type) (*storagepb.WriteStream, error) {
	path, err := e.table(ctx, stringSchema)
	if err != nil {
		return nil, err
	}
	return e.write.CreateWriteStream(ctx, &storagepb.CreateWriteStreamRequest{Parent: path,
		WriteStream: &storagepb.WriteStream{Type: kind}})
}

// session creates a read session on the table at path.
func (e *env) session(ctx context.Context, path string, format storagepb.DataFormat,
	options *storagepb.ReadSession_TableReadOptions, modifiers *storagepb.ReadSession_TableModifiers) (*storagepb.ReadSession, error) {
	return e.read.CreateReadSession(ctx, &storagepb.CreateReadSessionRequest{Parent: "projects/" + project,
		ReadSession: &storagepb.ReadSession{Table: path, DataFormat: format, ReadOptions: options, TableModifiers: modifiers}})
}

// readAll reads every stream of a session on a table holding a row, from offset, with the extra
// fields of the request, if any, encoded as unknown fields.
func (e *env) readAll(ctx context.Context, format storagepb.DataFormat, offset int64, extra []byte) error {
	path, err := e.filledTable(ctx)
	if err != nil {
		return err
	}
	session, err := e.session(ctx, path, format, nil, nil)
	if err != nil {
		return err
	}
	for _, stream := range session.Streams {
		request := &storagepb.ReadRowsRequest{ReadStream: stream.Name, Offset: offset}
		request.ProtoReflect().SetUnknown(extra)
		rows, err := e.read.ReadRows(ctx, request)
		if err != nil {
			return err
		}
		for {
			if _, err := rows.Recv(); err == io.EOF {
				break
			} else if err != nil {
				return err
			}
		}
	}
	return nil
}

// readSession creates a session on a table of the schema.
func readSession(schema bigquery.Schema, format storagepb.DataFormat, options *storagepb.ReadSession_TableReadOptions,
	modifiers *storagepb.ReadSession_TableModifiers) func(context.Context, *env) error {
	return func(ctx context.Context, e *env) error {
		path, err := e.table(ctx, schema)
		if err != nil {
			return err
		}
		_, err = e.session(ctx, path, format, options, modifiers)
		return err
	}
}

// appendTo appends value to the STRING field `value` of the default stream of a new table of the
// schema.
func appendTo(schema bigquery.Schema, value string) func(context.Context, *env) error {
	return func(ctx context.Context, e *env) error {
		path, err := e.table(ctx, schema)
		if err != nil {
			return err
		}
		return e.send(ctx, &storagepb.AppendRowsRequest{WriteStream: path + "/streams/_default",
			Rows: &storagepb.AppendRowsRequest_ProtoRows{ProtoRows: &storagepb.AppendRowsRequest_ProtoData{
				WriterSchema: &storagepb.ProtoSchema{ProtoDescriptor: &descriptorpb.DescriptorProto{
					Name: proto.String("Row"), Field: []*descriptorpb.FieldDescriptorProto{stringField("value")}}},
				Rows: &storagepb.ProtoRows{SerializedRows: [][]byte{append([]byte{0x0a, byte(len(value))}, value...)}}}}})
	}
}

func stringField(name string) *descriptorpb.FieldDescriptorProto {
	return &descriptorpb.FieldDescriptorProto{Name: proto.String(name), Number: proto.Int32(1),
		Type: descriptorpb.FieldDescriptorProto_TYPE_STRING.Enum(), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum()}
}

// createStream creates a write stream of the type.
func createStream(kind storagepb.WriteStream_Type) func(context.Context, *env) error {
	return func(ctx context.Context, e *env) error {
		_, err := e.stream(ctx, kind)
		return err
	}
}

// getStream gets a COMMITTED stream with the view.
func getStream(view storagepb.WriteStreamView) func(context.Context, *env) error {
	return func(ctx context.Context, e *env) error {
		stream, err := e.stream(ctx, storagepb.WriteStream_COMMITTED)
		if err != nil {
			return err
		}
		_, err = e.write.GetWriteStream(ctx, &storagepb.GetWriteStreamRequest{Name: stream.Name, View: view})
		return err
	}
}

// finalize finalizes a stream of the type that holds a row, times times.
func finalize(e *env, ctx context.Context, kind storagepb.WriteStream_Type, times int) (*storagepb.WriteStream, error) {
	stream, err := e.stream(ctx, kind)
	if err != nil {
		return nil, err
	}
	if err := e.append(ctx, stream.Name, nil); err != nil {
		return nil, err
	}
	for range times {
		if _, err := e.write.FinalizeWriteStream(ctx, &storagepb.FinalizeWriteStreamRequest{Name: stream.Name}); err != nil {
			return nil, err
		}
	}
	return stream, nil
}

var sections = []section{
	{"BigQueryRead", []rpc{
		{"CreateReadSession", "See [Reading rows](storage-api.md#reading-rows).", []form{
			{"Arrow", readSession(stringSchema, storagepb.DataFormat_ARROW, nil, nil)},
			{"Avro", readSession(stringSchema, storagepb.DataFormat_AVRO, nil, nil)},
			{"selected fields", readSession(stringSchema, storagepb.DataFormat_ARROW,
				&storagepb.ReadSession_TableReadOptions{SelectedFields: []string{"value"}}, nil)},
			{"row restrictions", readSession(stringSchema, storagepb.DataFormat_ARROW,
				&storagepb.ReadSession_TableReadOptions{RowRestriction: "value = 'probe'"}, nil)},
			{"historical snapshots", readSession(stringSchema, storagepb.DataFormat_ARROW, nil,
				&storagepb.ReadSession_TableModifiers{SnapshotTime: timestamppb.New(time.Now().Add(-time.Minute))})},
			{"sampling", readSession(stringSchema, storagepb.DataFormat_ARROW,
				&storagepb.ReadSession_TableReadOptions{SamplePercentage: proto.Float64(50)}, nil)},
			{"response compression", readSession(stringSchema, storagepb.DataFormat_ARROW,
				&storagepb.ReadSession_TableReadOptions{ResponseCompressionCodec: storagepb.ReadSession_TableReadOptions_RESPONSE_COMPRESSION_CODEC_LZ4.Enum()}, nil)},
			{"Arrow buffer compression", readSession(stringSchema, storagepb.DataFormat_ARROW,
				&storagepb.ReadSession_TableReadOptions{OutputFormatSerializationOptions: &storagepb.ReadSession_TableReadOptions_ArrowSerializationOptions{
					ArrowSerializationOptions: &storagepb.ArrowSerializationOptions{BufferCompression: storagepb.ArrowSerializationOptions_LZ4_FRAME}}}, nil)},
			{"RANGE columns", readSession(bigquery.Schema{{Name: "r", Type: bigquery.RangeFieldType,
				RangeElementType: &bigquery.RangeElementType{Type: bigquery.DateFieldType}}}, storagepb.DataFormat_AVRO, nil, nil)},
			{"JSON columns in Arrow", readSession(bigquery.Schema{{Name: "j", Type: bigquery.JSONFieldType}},
				storagepb.DataFormat_ARROW, nil, nil)},
		}},
		{"ReadRows", "", []form{
			{"Arrow", func(ctx context.Context, e *env) error { return e.readAll(ctx, storagepb.DataFormat_ARROW, 0, nil) }},
			{"Avro", func(ctx context.Context, e *env) error { return e.readAll(ctx, storagepb.DataFormat_AVRO, 0, nil) }},
			{"offsets", func(ctx context.Context, e *env) error { return e.readAll(ctx, storagepb.DataFormat_ARROW, 1, nil) }},
			// The Go client predates ReadRowsRequest.arrow_serialization_options, field 5.
			{"serialization options", func(ctx context.Context, e *env) error {
				return e.readAll(ctx, storagepb.DataFormat_ARROW, 0, protowire.AppendBytes(protowire.AppendTag(nil, 5, protowire.BytesType), nil))
			}},
		}},
		{"SplitReadStream", "", []form{
			{"fractions", func(ctx context.Context, e *env) error {
				path, err := e.filledTable(ctx)
				if err != nil {
					return err
				}
				session, err := e.session(ctx, path, storagepb.DataFormat_ARROW, nil, nil)
				if err != nil {
					return err
				}
				_, err = e.read.SplitReadStream(ctx, &storagepb.SplitReadStreamRequest{Name: session.Streams[0].Name, Fraction: 0.5})
				return err
			}},
		}},
	}},
	{"BigQueryWrite", []rpc{
		{"CreateWriteStream", "See [Lifetime and persistence](storage-api.md#lifetime-and-persistence).", []form{
			{"COMMITTED", createStream(storagepb.WriteStream_COMMITTED)},
			{"BUFFERED", createStream(storagepb.WriteStream_BUFFERED)},
			{"PENDING", createStream(storagepb.WriteStream_PENDING)},
		}},
		{"AppendRows", "See [Write limitations](storage-api.md#write-limitations).", []form{
			{"default streams", func(ctx context.Context, e *env) error {
				_, err := e.filledTable(ctx)
				return err
			}},
			{"explicit streams with offsets", func(ctx context.Context, e *env) error {
				stream, err := e.stream(ctx, storagepb.WriteStream_COMMITTED)
				if err != nil {
					return err
				}
				return e.append(ctx, stream.Name, wrapperspb.Int64(0))
			}},
			{"Arrow rows", func(ctx context.Context, e *env) error {
				path, err := e.table(ctx, stringSchema)
				if err != nil {
					return err
				}
				return e.send(ctx, &storagepb.AppendRowsRequest{WriteStream: path + "/streams/_default",
					Rows: &storagepb.AppendRowsRequest_ArrowRows{ArrowRows: &storagepb.AppendRowsRequest_ArrowData{}}})
			}},
			{"writer schema changes on a connection", func(ctx context.Context, e *env) error {
				path, err := e.table(ctx, bigquery.Schema{{Name: "value", Type: bigquery.StringFieldType},
					{Name: "other", Type: bigquery.StringFieldType}})
				if err != nil {
					return err
				}
				request := func(field string) *storagepb.AppendRowsRequest {
					return &storagepb.AppendRowsRequest{WriteStream: path + "/streams/_default",
						Rows: &storagepb.AppendRowsRequest_ProtoRows{ProtoRows: &storagepb.AppendRowsRequest_ProtoData{
							WriterSchema: &storagepb.ProtoSchema{ProtoDescriptor: &descriptorpb.DescriptorProto{
								Name: proto.String("Row"), Field: []*descriptorpb.FieldDescriptorProto{stringField(field)}}},
							Rows: &storagepb.ProtoRows{SerializedRows: [][]byte{row}}}}}
				}
				return e.send(ctx, request("value"), request("other"))
			}},
			{"column defaults", appendTo(bigquery.Schema{{Name: "value", Type: bigquery.StringFieldType,
				DefaultValueExpression: "'x'"}}, "probe")},
			{"parameterized columns", appendTo(bigquery.Schema{{Name: "value", Type: bigquery.StringFieldType,
				MaxLength: 10}}, "probe")},
			{"JSON columns", appendTo(bigquery.Schema{{Name: "value", Type: bigquery.JSONFieldType}}, `{"a": 1}`)},
			{"GEOGRAPHY columns", appendTo(bigquery.Schema{{Name: "value", Type: bigquery.GeographyFieldType}}, "POINT(1 2)")},
		}},
		{"GetWriteStream", "See [Write limitations](storage-api.md#write-limitations).", []form{
			{"BASIC", getStream(storagepb.WriteStreamView_BASIC)},
			{"FULL", getStream(storagepb.WriteStreamView_FULL)},
		}},
		{"FinalizeWriteStream", "", []form{
			{"explicit streams", func(ctx context.Context, e *env) error {
				_, err := finalize(e, ctx, storagepb.WriteStream_PENDING, 1)
				return err
			}},
			{"repeated finalization", func(ctx context.Context, e *env) error {
				_, err := finalize(e, ctx, storagepb.WriteStream_PENDING, 2)
				return err
			}},
		}},
		{"FlushRows", "", []form{
			{"BUFFERED streams", func(ctx context.Context, e *env) error {
				stream, err := e.stream(ctx, storagepb.WriteStream_BUFFERED)
				if err != nil {
					return err
				}
				if err := e.append(ctx, stream.Name, nil); err != nil {
					return err
				}
				_, err = e.write.FlushRows(ctx, &storagepb.FlushRowsRequest{WriteStream: stream.Name, Offset: wrapperspb.Int64(0)})
				return err
			}},
		}},
		{"BatchCommitWriteStreams", "", []form{
			{"finalized PENDING streams", func(ctx context.Context, e *env) error {
				stream, err := finalize(e, ctx, storagepb.WriteStream_PENDING, 1)
				if err != nil {
					return err
				}
				parent, _, _ := strings.Cut(stream.Name, "/streams/")
				response, err := e.write.BatchCommitWriteStreams(ctx, &storagepb.BatchCommitWriteStreamsRequest{
					Parent: parent, WriteStreams: []string{stream.Name}})
				if err == nil && len(response.StreamErrors) > 0 {
					err = fmt.Errorf("stream errors: %v", response.StreamErrors)
				}
				return err
			}},
		}},
	}},
}

func main() {
	binary := flag.String("emulator", "bazel-bin/bigquery-emulator-duckdb", "emulator binary")
	flag.Parse()
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()
	if err := run(ctx, *binary, os.Stdout); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func run(ctx context.Context, binary string, w io.Writer) error {
	p, err := emulatorprocess.Start(ctx, binary, "", "--host", "127.0.0.1", "--port", "0",
		"--project", `{"projectId":"`+project+`"}`)
	if err != nil {
		return err
	}
	defer p.Stop()
	rest, err := bigquery.NewClient(ctx, project, option.WithEndpoint(p.URL), option.WithoutAuthentication())
	if err != nil {
		return err
	}
	defer rest.Close()
	if err := rest.Dataset("probe").Create(ctx, nil); err != nil {
		return err
	}
	conn, err := grpc.NewClient(p.GRPC, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return err
	}
	defer conn.Close()
	e := &env{rest: rest, read: storagepb.NewBigQueryReadClient(conn), write: storagepb.NewBigQueryWriteClient(conn)}

	var tallies []probereport.Tally
	var body []byte
	for _, s := range sections {
		t := probereport.Tally{Title: s.title, Counts: map[string]int{}}
		body = fmt.Appendf(body, "## %s\n\n| RPC | Status | Notes |\n| --- | --- | --- |\n", s.title)
		for _, r := range s.rpcs {
			counts := map[probereport.Outcome]int{}
			rejected := map[string]bool{}
			failures := map[string]bool{}
			for _, f := range r.forms {
				err := f.call(ctx, e)
				switch {
				case err == nil:
					counts[probereport.Runs]++
				case status.Code(err) == codes.Unimplemented:
					counts[probereport.Unsupported]++
					rejected[f.name] = true
				default:
					counts[probereport.Broken]++
					failures[f.name+": "+errorMessage(err)] = true
				}
			}
			st := probereport.Status(counts)
			t.Counts[st]++
			body = fmt.Appendf(body, "| `%s` | %s | %s |\n", r.name, st,
				probereport.Notes(r.note, st, rejected, failures))
		}
		tallies = append(tallies, t)
		body = append(body, '\n')
	}
	fmt.Fprint(w, preamble)
	probereport.WriteSummary(w, "Service", tallies)
	_, err = fmt.Fprint(w, "\n", string(body[:len(body)-1]))
	return err
}

// errorMessage names the gRPC code of err, if it has one.
func errorMessage(err error) string {
	if s, ok := status.FromError(err); ok {
		return s.Code().String() + ": " + s.Message()
	}
	return err.Error()
}
