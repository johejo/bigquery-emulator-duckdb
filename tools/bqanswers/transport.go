package main

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"net/textproto"
	"net/url"
	"strings"

	// Register BigQuery Storage Read/Write descriptors for dynamic protobuf JSON calls. No server
	// reflection is required, and both unary RPCs and streaming RPCs use the same case format.
	_ "cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/encoding/protojson"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/reflect/protoregistry"
	"google.golang.org/protobuf/types/dynamicpb"
)

type scenarioTransport struct {
	token        string
	client       *http.Client
	restEndpoint string
	grpcEndpoint string
	grpcOptions  []grpc.DialOption
	connection   *grpc.ClientConn
}

type bearerToken string

func (token bearerToken) GetRequestMetadata(context.Context, ...string) (map[string]string, error) {
	return map[string]string{"authorization": "Bearer " + string(token)}, nil
}
func (bearerToken) RequireTransportSecurity() bool { return true }

func newScenarioTransport(token string) *scenarioTransport {
	return &scenarioTransport{
		token: token, client: &http.Client{
			// Keep credentials on the configured host; BigQuery's API does not redirect requests.
			CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse },
		},
		restEndpoint: "https://bigquery.googleapis.com",
		grpcEndpoint: "bigquerystorage.googleapis.com:443",
		grpcOptions: []grpc.DialOption{
			grpc.WithTransportCredentials(credentials.NewTLS(&tls.Config{MinVersion: tls.VersionTLS12})),
			grpc.WithPerRPCCredentials(bearerToken(token)),
			grpc.WithDefaultCallOptions(grpc.MaxCallRecvMsgSize(129 * 1024 * 1024)),
		},
	}
}

func (transport *scenarioTransport) Close() error {
	if transport.connection != nil {
		return transport.connection.Close()
	}
	return nil
}

func (transport *scenarioTransport) call(ctx context.Context, step scenarioStep, vars map[string]any) (scenarioResponse, error) {
	if step.REST != nil {
		return transport.rest(ctx, *step.REST, vars)
	}
	return transport.rpc(ctx, *step.GRPC, vars)
}

// Expanded request objects are decoded strictly before transport. The template substitution
// never touches authorization metadata and cannot choose another REST host.
func expandedRequest[T any](request T, vars map[string]any) (T, error) {
	var result T
	encoded, err := json.Marshal(request)
	if err != nil {
		return result, err
	}
	var tree any
	decoder := json.NewDecoder(bytes.NewReader(encoded))
	decoder.UseNumber()
	if err := decoder.Decode(&tree); err != nil {
		return result, err
	}
	tree, err = expand(tree, vars)
	if err != nil {
		return result, err
	}
	encoded, err = json.Marshal(tree)
	if err != nil {
		return result, err
	}
	decoder = json.NewDecoder(bytes.NewReader(encoded))
	decoder.UseNumber()
	err = decoder.Decode(&result)
	return result, err
}

func (transport *scenarioTransport) rest(ctx context.Context, original restRequest, vars map[string]any) (scenarioResponse, error) {
	request, err := expandedRequest(original, vars)
	if err != nil {
		return scenarioResponse{}, err
	}
	path, err := url.Parse(request.Path)
	if err != nil {
		return scenarioResponse{}, err
	}
	if path.IsAbs() || path.Host != "" || !strings.HasPrefix(path.Path, "/") {
		return scenarioResponse{}, errors.New("REST requests require a path on the BigQuery API")
	}
	var body bytes.Buffer
	contentType := "application/json"
	if request.Upload != nil {
		writer := multipart.NewWriter(&body)
		for _, part := range []struct {
			kind  string
			value any
		}{{"application/json; charset=UTF-8", request.Body}, {"application/octet-stream", *request.Upload}} {
			partWriter, err := writer.CreatePart(textproto.MIMEHeader{"Content-Type": []string{part.kind}})
			if err != nil {
				return scenarioResponse{}, err
			}
			if text, ok := part.value.(string); ok && part.kind == "application/octet-stream" {
				_, err = io.WriteString(partWriter, text)
			} else {
				err = json.NewEncoder(partWriter).Encode(part.value)
			}
			if err != nil {
				return scenarioResponse{}, err
			}
		}
		if err := writer.Close(); err != nil {
			return scenarioResponse{}, err
		}
		contentType = "multipart/related; boundary=" + writer.Boundary()
	} else if request.Body != nil {
		if err := json.NewEncoder(&body).Encode(request.Body); err != nil {
			return scenarioResponse{}, err
		}
	}
	post, err := http.NewRequestWithContext(ctx, request.Method, transport.restEndpoint+path.String(), &body)
	if err != nil {
		return scenarioResponse{}, err
	}
	for name, value := range request.Headers {
		post.Header.Set(name, value)
	}
	post.Header.Set("Content-Type", contentType)
	if transport.token != "" {
		post.Header.Set("Authorization", "Bearer "+transport.token)
	}
	response, err := transport.client.Do(post)
	if err != nil {
		return scenarioResponse{}, err
	}
	defer response.Body.Close()
	var value any
	decoder := json.NewDecoder(response.Body)
	decoder.UseNumber()
	if err := decoder.Decode(&value); err != nil && err != io.EOF {
		return scenarioResponse{}, fmt.Errorf("HTTP %d: invalid JSON response: %w", response.StatusCode, err)
	}
	// Authentication, quota/availability and server failures leave a case unanswered. They are
	// operational failures, not evidence of the API behavior that a compatibility case checks.
	if response.StatusCode == 401 || response.StatusCode == 403 || response.StatusCode == 429 || response.StatusCode >= 500 {
		return scenarioResponse{}, fmt.Errorf("HTTP %d: %v", response.StatusCode, value)
	}
	return scenarioResponse{
		Value:  map[string]any{"status": response.StatusCode, "body": value},
		Failed: response.StatusCode < 200 || response.StatusCode >= 300,
	}, nil
}

func storageMethod(name string) (protoreflect.MethodDescriptor, error) {
	service, method, found := strings.Cut(strings.TrimPrefix(name, "/"), "/")
	if !found || method == "" {
		return nil, fmt.Errorf("RPC method must be Service/Method: %q", name)
	}
	if service != "google.cloud.bigquery.storage.v1.BigQueryRead" && service != "google.cloud.bigquery.storage.v1.BigQueryWrite" {
		return nil, fmt.Errorf("unsupported RPC service %q", service)
	}
	descriptor, err := protoregistry.GlobalFiles.FindDescriptorByName(protoreflect.FullName(service + "." + method))
	if err != nil {
		return nil, err
	}
	result, ok := descriptor.(protoreflect.MethodDescriptor)
	if !ok {
		return nil, fmt.Errorf("%q is not an RPC", name)
	}
	return result, nil
}

func (transport *scenarioTransport) rpc(ctx context.Context, original grpcRequest, vars map[string]any) (scenarioResponse, error) {
	request, err := expandedRequest(original, vars)
	if err != nil {
		return scenarioResponse{}, err
	}
	method, err := storageMethod(request.Method)
	if err != nil {
		return scenarioResponse{}, err
	}
	inputs := make([]*dynamicpb.Message, len(request.Requests))
	for i, input := range request.Requests {
		encoded, err := json.Marshal(input)
		if err != nil {
			return scenarioResponse{}, err
		}
		inputs[i] = dynamicpb.NewMessage(method.Input())
		if err := protojson.Unmarshal(encoded, inputs[i]); err != nil {
			return scenarioResponse{}, err
		}
	}
	if transport.connection == nil {
		transport.connection, err = grpc.NewClient(transport.grpcEndpoint, transport.grpcOptions...)
		if err != nil {
			return scenarioResponse{}, err
		}
	}
	headers := metadata.MD{}
	for name, value := range request.Headers {
		headers.Set(name, value)
	}
	ctx = metadata.NewOutgoingContext(ctx, headers)
	fullName := "/" + strings.TrimPrefix(request.Method, "/")
	messages := []any{}
	appendMessage := func(output *dynamicpb.Message) error {
		data, err := protojson.Marshal(output)
		if err != nil {
			return err
		}
		var value any
		decoder := json.NewDecoder(bytes.NewReader(data))
		decoder.UseNumber()
		if err := decoder.Decode(&value); err != nil {
			return err
		}
		messages = append(messages, value)
		return nil
	}
	var rpcErr error
	if !method.IsStreamingClient() && !method.IsStreamingServer() {
		output := dynamicpb.NewMessage(method.Output())
		rpcErr = transport.connection.Invoke(ctx, fullName, inputs[0], output)
		if rpcErr == nil {
			if err := appendMessage(output); err != nil {
				return scenarioResponse{}, err
			}
		}
	} else {
		streamCtx, cancel := context.WithCancel(ctx)
		defer cancel()
		stream, err := transport.connection.NewStream(streamCtx, &grpc.StreamDesc{ClientStreams: method.IsStreamingClient(), ServerStreams: method.IsStreamingServer()}, fullName)
		if err != nil {
			return scenarioResponse{}, err
		}
		sent := make(chan error, 1)
		// Receive while sending so a bidirectional stream cannot deadlock on flow control.
		go func() {
			for _, input := range inputs {
				if err := stream.SendMsg(input); err != nil {
					sent <- err
					return
				}
			}
			sent <- stream.CloseSend()
		}()
		for {
			output := dynamicpb.NewMessage(method.Output())
			err := stream.RecvMsg(output)
			if err == io.EOF {
				break
			}
			if err != nil {
				rpcErr = err
				break
			}
			if err := appendMessage(output); err != nil {
				cancel()
				<-sent
				return scenarioResponse{}, err
			}
		}
		cancel()
		sendErr := <-sent
		// RecvMsg reports the server's canonical status when SendMsg only reports EOF.
		if rpcErr == nil && sendErr != io.EOF {
			rpcErr = sendErr
		}
	}
	code := status.Code(rpcErr)
	switch code {
	case codes.Unauthenticated, codes.PermissionDenied, codes.ResourceExhausted, codes.Unavailable, codes.DeadlineExceeded, codes.Canceled, codes.Internal, codes.Unknown:
		return scenarioResponse{}, rpcErr
	}
	value := map[string]any{"code": code.String(), "messages": messages}
	if rpcErr != nil {
		encoded, err := json.Marshal(status.Convert(rpcErr).Proto())
		if err != nil {
			return scenarioResponse{}, err
		}
		var detail any
		if err := json.Unmarshal(encoded, &detail); err != nil {
			return scenarioResponse{}, err
		}
		value["error"] = detail
	}
	return scenarioResponse{Value: value, Failed: rpcErr != nil}, nil
}
