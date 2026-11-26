package gcsclient

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"reflect"
	"sort"
	"strings"
	"sync"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// gcsObject is an object of the controlled Storage server, in the JSON API's resource format.
type gcsObject struct {
	Name        string            `json:"name"`
	Bucket      string            `json:"bucket"`
	Generation  string            `json:"generation"`
	ContentType string            `json:"contentType,omitempty"`
	Size        string            `json:"size"`
	MD5Hash     string            `json:"md5Hash,omitempty"`
	Updated     string            `json:"updated"`
	Metadata    map[string]string `json:"metadata,omitempty"`
}

// objectServer lists and describes the objects of the bucket `bucket`.
type objectServer struct {
	mu      sync.Mutex
	objects map[string]gcsObject
}

func (s *objectServer) put(object gcsObject) {
	s.mu.Lock()
	defer s.mu.Unlock()
	object.Bucket = "bucket"
	s.objects[object.Name] = object
}

func (s *objectServer) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	defer s.mu.Unlock()
	w.Header().Set("Content-Type", "application/json")
	if r.URL.Path == "/storage/v1/b/bucket/o" {
		var items []gcsObject
		for name, object := range s.objects {
			if strings.HasPrefix(name, r.URL.Query().Get("prefix")) {
				items = append(items, object)
			}
		}
		sort.Slice(items, func(i, j int) bool { return items[i].Name < items[j].Name })
		_ = json.NewEncoder(w).Encode(map[string]any{"kind": "storage#objects", "items": items})
		return
	}
	if object, ok := s.objects[strings.TrimPrefix(r.URL.Path, "/storage/v1/b/bucket/o/")]; ok {
		_ = json.NewEncoder(w).Encode(object)
		return
	}
	http.NotFound(w, r)
}

func objectTableRows(t *testing.T, client *bigquery.Client, query string) [][]bigquery.Value {
	t.Helper()
	q := client.Query(query)
	q.DisableQueryCache = true
	it, err := q.Read(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	var rows [][]bigquery.Value
	for {
		var row []bigquery.Value
		if err := it.Next(&row); err == iterator.Done {
			break
		} else if err != nil {
			t.Fatal(err)
		}
		rows = append(rows, row)
	}
	return rows
}

// An object table lists the objects of its URIs, with the fixed schema of
// third_party/bigquery_docs/object-table-introduction.md, whenever a query reads it, since it has
// no metadata cache.
func TestObjectTable(t *testing.T) {
	configure(t)
	objects := &objectServer{objects: map[string]gcsObject{}}
	objects.put(gcsObject{
		Name:        "images/a.jpeg",
		Generation:  "1658420140148000",
		ContentType: "image/jpeg",
		Size:        "26797",
		MD5Hash:     "jDO+EP/MnJ3+V+vQi3WEeQ==",
		Updated:     "2022-07-21T17:35:40.148Z",
	})
	objects.put(gcsObject{
		Name:        "images/b.bmp",
		Generation:  "1652530178114000",
		ContentType: "image/bmp",
		Size:        "57932",
		MD5Hash:     "ROuQzRsaJJNuVkR1uQQXpw==",
		Updated:     "2022-05-14T12:09:38.114Z",
		Metadata:    map[string]string{"no_pii": "true"},
	})
	objects.put(gcsObject{Name: "other/c.txt", Generation: "1", Size: "1", Updated: "2022-01-01T00:00:00Z"})
	server := httptest.NewServer(objects)
	defer server.Close()
	t.Setenv("STORAGE_EMULATOR_HOST", server.URL)
	client := startWith(t, "--connection=test.us.storage=cloud_resource")
	ctx := t.Context()
	table := client.Dataset("gcs").Table("objects")
	if err := table.Create(ctx, &bigquery.TableMetadata{
		ExternalDataConfig: &bigquery.ExternalDataConfig{
			SourceURIs:     []string{"gs://bucket/images/*"},
			ConnectionID:   "test.us.storage",
			ObjectMetadata: bigquery.ObjectMetadataSimple,
		},
	}); err != nil {
		t.Fatal(err)
	}
	metadata, err := table.Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if metadata.Type != bigquery.ExternalTable || metadata.ExternalDataConfig == nil ||
		metadata.ExternalDataConfig.ObjectMetadata != bigquery.ObjectMetadataSimple ||
		metadata.ExternalDataConfig.ConnectionID != "test.us.storage" ||
		!reflect.DeepEqual(metadata.ExternalDataConfig.SourceURIs, []string{"gs://bucket/images/*"}) {
		t.Fatalf("object table metadata = %+v", metadata)
	}
	// The ObjectRef fields of ref are those of third_party/bigquery_docs/reference/standard-sql/objectref_functions.md.
	var schema []string
	for _, field := range metadata.Schema {
		name := field.Name + " " + string(field.Type)
		if field.Repeated {
			name += " REPEATED"
		}
		for _, child := range field.Schema {
			name += " " + child.Name + ":" + string(child.Type)
		}
		schema = append(schema, name)
	}
	want := []string{
		"uri STRING",
		"generation INTEGER",
		"content_type STRING",
		"size INTEGER",
		"md5_hash STRING",
		"updated TIMESTAMP",
		"metadata RECORD REPEATED name:STRING value:STRING",
		"ref RECORD uri:STRING version:STRING authorizer:STRING details:JSON",
	}
	if !reflect.DeepEqual(schema, want) {
		t.Fatalf("schema = %q, want %q", schema, want)
	}

	query := "SELECT uri, generation, content_type, size, md5_hash, updated, " +
		"ARRAY(SELECT CONCAT(name, '=', value) FROM UNNEST(metadata)), ref.uri AS ref_uri, ref.version " +
		"FROM gcs.objects ORDER BY uri"
	updated := func(text string) time.Time {
		value, err := time.Parse(time.RFC3339Nano, text)
		if err != nil {
			t.Fatal(err)
		}
		return value
	}
	got := objectTableRows(t, client, query)
	wantRows := [][]bigquery.Value{
		{"gs://bucket/images/a.jpeg", int64(1658420140148000), "image/jpeg", int64(26797),
			"8c33be10ffcc9c9dfe57ebd08b758479", updated("2022-07-21T17:35:40.148Z"), []bigquery.Value(nil),
			"gs://bucket/images/a.jpeg", "1658420140148000"},
		{"gs://bucket/images/b.bmp", int64(1652530178114000), "image/bmp", int64(57932),
			"44eb90cd1b1a24936e564475b90417a7", updated("2022-05-14T12:09:38.114Z"), []bigquery.Value{"no_pii=true"},
			"gs://bucket/images/b.bmp", "1652530178114000"},
	}
	for _, row := range got {
		row[5] = row[5].(time.Time).UTC()
	}
	if !reflect.DeepEqual(got, wantRows) {
		t.Fatalf("rows = %v, want %v", got, wantRows)
	}

	// The documentation's filter on custom metadata.
	got = objectTableRows(t, client, "SELECT uri FROM gcs.objects "+
		"WHERE ARRAY_LENGTH(metadata) = 1 AND metadata[OFFSET(0)].name = 'no_pii'")
	if !reflect.DeepEqual(got, [][]bigquery.Value{{"gs://bucket/images/b.bmp"}}) {
		t.Fatalf("filtered rows = %v", got)
	}

	objects.put(gcsObject{Name: "images/d.png", Generation: "2", Size: "3", Updated: "2023-01-01T00:00:00Z"})
	got = objectTableRows(t, client, "SELECT uri FROM gcs.objects ORDER BY uri")
	if !reflect.DeepEqual(got, [][]bigquery.Value{
		{"gs://bucket/images/a.jpeg"}, {"gs://bucket/images/b.bmp"}, {"gs://bucket/images/d.png"},
	}) {
		t.Fatalf("rows after an upload = %v", got)
	}

	// The connection is in US, unlike the dataset.
	if err := client.Dataset("gcs_eu").Create(ctx, &bigquery.DatasetMetadata{Location: "EU"}); err != nil {
		t.Fatal(err)
	}
	if err := client.Dataset("gcs_eu").Table("objects").Create(ctx, &bigquery.TableMetadata{
		ExternalDataConfig: &bigquery.ExternalDataConfig{
			SourceURIs:     []string{"gs://bucket/images/*"},
			ConnectionID:   "test.us.storage",
			ObjectMetadata: bigquery.ObjectMetadataSimple,
		},
	}); err == nil {
		t.Error("an object table in EU with a connection in US was created")
	}

	// Object tables are read-only.
	for _, sql := range []string{
		"DELETE FROM gcs.objects WHERE TRUE",
		"INSERT INTO gcs.objects (uri) VALUES ('gs://bucket/images/e.png')",
	} {
		if err := externalDDL(ctx, client, sql); err == nil {
			t.Errorf("%s succeeded", sql)
		}
	}
}
