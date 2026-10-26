package bqscenario

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"mime"
	"mime/multipart"
	"net"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"google.golang.org/grpc/test/bufconn"
	"google.golang.org/protobuf/types/known/anypb"
	"google.golang.org/protobuf/types/known/wrapperspb"
)

func TestRESTScenarioUploadAndResponse(t *testing.T) {
	for _, binary := range []bool{false, true} {
		t.Run(fmt.Sprint("binary=", binary), func(t *testing.T) {
			mediaBytes := "2.25,2.5\n"
			if binary {
				mediaBytes = "PAR1\x00\xff\x80\r\nPAR1"
			}
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.Header.Get("Authorization") != "Bearer token" {
					t.Error("missing authorization")
				}
				if r.URL.Path != "/upload/bigquery/v2/projects/project/jobs" || r.URL.Query().Get("uploadType") != "multipart" {
					t.Errorf("URL = %s", r.URL)
				}
				kind, params, err := mime.ParseMediaType(r.Header.Get("Content-Type"))
				if err != nil || kind != "multipart/related" {
					t.Errorf("content type = %s, %v", kind, err)
					w.WriteHeader(500)
					return
				}
				reader := multipart.NewReader(r.Body, params["boundary"])
				part, err := reader.NextPart()
				if err != nil {
					t.Error(err)
					w.WriteHeader(500)
					return
				}
				data, _ := io.ReadAll(part)
				if !strings.Contains(string(data), `9007199254740993`) || !strings.Contains(string(data), `quoted\"\\value`) {
					t.Errorf("metadata = %s", data)
				}
				part, err = reader.NextPart()
				if err != nil {
					t.Error(err)
					w.WriteHeader(500)
					return
				}
				data, _ = io.ReadAll(part)
				if string(data) != mediaBytes {
					t.Errorf("upload = %q", data)
				}
				if _, err := reader.NextPart(); err != io.EOF {
					t.Errorf("extra part = %v", err)
				}
				w.Header().Set("Content-Type", "application/json")
				_, _ = io.WriteString(w, `{"number":9007199254740993,"jobReference":{"jobId":"job"}}`)
			}))
			defer server.Close()
			transport := NewTransport("token")
			transport.RESTEndpoint = server.URL
			media := "${data}"
			request := &RESTRequest{
				Method: "POST", Path: "/upload/bigquery/v2/projects/${project}/jobs?uploadType=multipart",
				Body: map[string]any{"number": "${number}", "text": "${text}"}, Upload: &media,
			}
			data := mediaBytes
			if binary {
				request.Upload = nil
				request.UploadBase64 = &media
				data = base64.StdEncoding.EncodeToString([]byte(mediaBytes))
			}
			response, err := transport.Call(t.Context(), Step{REST: request}, map[string]any{"project": "project", "number": json.Number("9007199254740993"), "text": "quoted\"\\value", "data": data})
			if err != nil || response.Failed {
				t.Fatalf("response = %+v, %v", response, err)
			}
			got, _, err := pointer(response.Value, "/body/number")
			if err != nil || got != json.Number("9007199254740993") {
				t.Fatalf("number = %v, %v", got, err)
			}
		})
	}
}

func TestRESTScenarioInvalidUpload(t *testing.T) {
	invalid, valid := "not base64!", "UEFSMQ=="
	for _, request := range []RESTRequest{
		{Method: "POST", Path: "/upload", UploadBase64: &invalid},
		{Method: "POST", Path: "/upload", Upload: &valid, UploadBase64: &valid},
	} {
		if _, err := NewTransport("").rest(t.Context(), request, nil); err == nil {
			t.Fatal("invalid upload accepted")
		}
	}
}

func TestRESTScenarioErrorClassification(t *testing.T) {
	for _, tc := range []struct {
		status              int
		failed, operational bool
	}{
		{200, false, false}, {204, false, false}, {400, true, false}, {404, true, false},
		{401, false, true}, {403, false, true}, {429, false, true}, {503, false, true},
	} {
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(tc.status)
			if tc.status != 204 {
				_, _ = io.WriteString(w, `{"error":{"message":"result"}}`)
			}
		}))
		transport := NewTransport("")
		transport.RESTEndpoint = server.URL
		got, err := transport.rest(t.Context(), RESTRequest{Method: "GET", Path: "/result"}, nil)
		server.Close()
		if (err != nil) != tc.operational || got.Failed != tc.failed {
			t.Errorf("status %d = %+v, %v", tc.status, got, err)
		}
	}
}

type answerStorageServer struct {
	storagepb.UnimplementedBigQueryWriteServer
}

func (answerStorageServer) CreateWriteStream(ctx context.Context, request *storagepb.CreateWriteStreamRequest) (*storagepb.WriteStream, error) {
	headers, _ := metadata.FromIncomingContext(ctx)
	if len(headers.Get("x-goog-request-params")) != 1 {
		return nil, status.Error(codes.InvalidArgument, "missing routing header")
	}
	switch request.Parent {
	case "invalid":
		failure := status.New(codes.InvalidArgument, "invalid parent").Proto()
		failure.Details = []*anypb.Any{{TypeUrl: "type.googleapis.com/unknown.Detail", Value: []byte{8, 1}}}
		return nil, status.FromProto(failure).Err()
	case "unavailable":
		return nil, status.Error(codes.Unavailable, "try later")
	}
	return &storagepb.WriteStream{Name: request.Parent + "/streams/created", Type: request.WriteStream.Type}, nil
}
func (answerStorageServer) AppendRows(stream storagepb.BigQueryWrite_AppendRowsServer) error {
	for {
		request, err := stream.Recv()
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return err
		}
		if request.WriteStream == "invalid" {
			return status.Error(codes.InvalidArgument, "stream rejected")
		}
		// Respond before the next receive, exercising bidirectional send/receive rather than a unary
		// stand-in. Offsets above 2^53 must retain their exact protobuf JSON representation.
		if err := stream.Send(&storagepb.AppendRowsResponse{Response: &storagepb.AppendRowsResponse_AppendResult_{
			AppendResult: &storagepb.AppendRowsResponse_AppendResult{Offset: wrapperspb.Int64(request.Offset.GetValue())},
		}}); err != nil {
			return err
		}
	}
}

type answerReadServer struct {
	storagepb.UnimplementedBigQueryReadServer
}

func (answerReadServer) ReadRows(request *storagepb.ReadRowsRequest, stream storagepb.BigQueryRead_ReadRowsServer) error {
	for _, n := range []int64{1, 2} {
		if err := stream.Send(&storagepb.ReadRowsResponse{RowCount: n}); err != nil {
			return err
		}
	}
	return nil
}

func mockGRPC(t *testing.T) *Transport {
	t.Helper()
	listener := bufconn.Listen(1024 * 1024)
	server := grpc.NewServer()
	storagepb.RegisterBigQueryWriteServer(server, answerStorageServer{})
	storagepb.RegisterBigQueryReadServer(server, answerReadServer{})
	go func() { _ = server.Serve(listener) }()
	t.Cleanup(server.Stop)
	transport := &Transport{Collecting: true, GRPCEndpoint: "passthrough:///local", GRPCOptions: []grpc.DialOption{
		grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithContextDialer(func(context.Context, string) (net.Conn, error) { return listener.Dial() }),
	}}
	t.Cleanup(func() { _ = transport.Close() })
	return transport
}

func TestGRPCScenarioUnaryAndErrors(t *testing.T) {
	transport := mockGRPC(t)
	for _, tc := range []struct {
		parent              string
		failed, operational bool
	}{
		{"parent", false, false}, {"invalid", true, false}, {"unavailable", false, true},
	} {
		response, err := transport.rpc(t.Context(), GRPCRequest{
			Method:   "google.cloud.bigquery.storage.v1.BigQueryWrite/CreateWriteStream",
			Headers:  map[string]string{"x-goog-request-params": "parent=${parent}"},
			Requests: []any{map[string]any{"parent": "${parent}", "writeStream": map[string]any{"type": "PENDING"}}},
		}, map[string]any{"parent": tc.parent})
		if (err != nil) != tc.operational || response.Failed != tc.failed {
			t.Fatalf("%s = %+v, %v", tc.parent, response, err)
		}
		if tc.parent == "invalid" {
			detail, present, err := pointer(response.Value, "/error/details/0/type_url")
			if err != nil || !present || detail != "type.googleapis.com/unknown.Detail" {
				t.Fatalf("unknown error detail = %v, %v", detail, err)
			}
		}
		if tc.parent == "parent" {
			name, _, err := pointer(response.Value, "/messages/0/name")
			if err != nil || name != "parent/streams/created" {
				t.Fatalf("name = %v, %v", name, err)
			}
		}
	}
	// Malformed protobuf JSON is a tool error; it must not become an answer about BigQuery.
	if _, err := transport.rpc(t.Context(), GRPCRequest{
		Method:   "google.cloud.bigquery.storage.v1.BigQueryWrite/CreateWriteStream",
		Requests: []any{map[string]any{"typo": true}},
	}, nil); err == nil {
		t.Fatal("unknown protobuf field accepted")
	}
}

func TestGRPCScenarioStreams(t *testing.T) {
	transport := mockGRPC(t)
	response, err := transport.rpc(t.Context(), GRPCRequest{
		Method: "google.cloud.bigquery.storage.v1.BigQueryWrite/AppendRows",
		Requests: []any{
			map[string]any{"writeStream": "stream", "offset": "9007199254740993"},
			map[string]any{"offset": "9007199254740994"},
		},
	}, nil)
	if err != nil || response.Failed {
		t.Fatalf("append = %+v, %v", response, err)
	}
	for i, path := range []string{"/messages/0/appendResult/offset", "/messages/1/appendResult/offset"} {
		got, _, err := pointer(response.Value, path)
		if err != nil || got != []string{"9007199254740993", "9007199254740994"}[i] {
			t.Errorf("offset = %v, %v", got, err)
		}
	}
	response, err = transport.rpc(t.Context(), GRPCRequest{
		Method:   "google.cloud.bigquery.storage.v1.BigQueryWrite/AppendRows",
		Requests: []any{map[string]any{"writeStream": "invalid"}},
	}, nil)
	if err != nil || !response.Failed {
		t.Fatalf("stream error = %+v, %v", response, err)
	}
	response, err = transport.rpc(t.Context(), GRPCRequest{
		Method: "google.cloud.bigquery.storage.v1.BigQueryRead/ReadRows", Requests: []any{map[string]any{"readStream": "stream"}},
	}, nil)
	if err != nil || response.Failed {
		t.Fatalf("read stream = %+v, %v", response, err)
	}
	messages, _, _ := pointer(response.Value, "/messages")
	want := []any{map[string]any{"rowCount": "1"}, map[string]any{"rowCount": "2"}}
	if !reflect.DeepEqual(messages, want) {
		t.Fatalf("server stream = %v", messages)
	}
}

func TestEmulatorTransportRetainsOperationalResponses(t *testing.T) {
	transport := mockGRPC(t)
	transport.Collecting = false
	response, err := transport.rpc(t.Context(), GRPCRequest{
		Method:   "google.cloud.bigquery.storage.v1.BigQueryWrite/CreateWriteStream",
		Requests: []any{map[string]any{"parent": "unavailable"}},
	}, nil)
	if err != nil || !response.Failed {
		t.Fatalf("emulator response: %+v, %v", response, err)
	}
}
