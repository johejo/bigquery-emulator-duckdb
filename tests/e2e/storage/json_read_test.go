package storage

import (
	"bytes"
	"encoding/json"
	"testing"

	"cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"github.com/apache/arrow-go/v18/arrow"
	"github.com/apache/arrow-go/v18/arrow/array"
	"github.com/apache/arrow-go/v18/arrow/ipc"
)

// The Storage Read type table specifies Utf8 for JSON, distinct from a NULL slot.
// third_party/bigquery_docs/reference/storage.md
func TestReadArrowJSON(t *testing.T) {
	t.Parallel()
	rest, read, _ := clients(t)
	path := table(t, rest, "storage_read_arrow_json", "sql_null JSON, json_null JSON, items ARRAY<JSON>, rec STRUCT<j JSON>")
	query(t, rest, `INSERT INTO storage_read_arrow_json.rows VALUES
		(NULL, JSON 'null', [JSON '{"text":"日本語"}'], STRUCT(JSON 'true' AS j))`)
	s := session(t, read, path, storagepb.DataFormat_ARROW, nil, 1)
	blocks := responses(t, read, s.Streams[0].Name, 0)
	if len(blocks) != 1 || blocks[0].GetRowCount() != 1 {
		t.Fatalf("expected one row in one block, got %v", blocks)
	}
	data := append(bytes.Clone(s.GetArrowSchema().GetSerializedSchema()), blocks[0].GetArrowRecordBatch().GetSerializedRecordBatch()...)
	r, err := ipc.NewReader(bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	defer r.Release()
	// Recorded from BigQuery's CreateReadSession (run nbowrkehtupcjzev43zudhhfmi):
	// the extension is on the list field, not on its nullable Utf8 item field.
	metadata := arrow.NewMetadata([]string{"ARROW:extension:name"}, []string{"google:sqlType:json"})
	jsonField := func(name string) arrow.Field {
		return arrow.Field{Name: name, Type: arrow.BinaryTypes.String, Nullable: true, Metadata: metadata}
	}
	want := arrow.NewSchema([]arrow.Field{
		jsonField("sql_null"), jsonField("json_null"),
		{Name: "items", Type: arrow.ListOf(arrow.BinaryTypes.String), Metadata: metadata},
		{Name: "rec", Type: arrow.StructOf(jsonField("j")), Nullable: true},
	}, nil)
	if !r.Schema().Equal(want) {
		t.Fatalf("Arrow JSON schema = %v, want %v", r.Schema(), want)
	}
	if !r.Next() {
		t.Fatalf("no record batch: %v", r.Err())
	}
	batch := r.RecordBatch()
	if !batch.Column(0).IsNull(0) {
		t.Error("SQL NULL has a non-null Arrow slot")
	}
	if c := batch.Column(1).(*array.String); c.IsNull(0) || c.Value(0) != "null" {
		t.Errorf("JSON null = %v, want the string null", c)
	}
	items := batch.Column(2).(*array.List).ListValues().(*array.String)
	var obj map[string]string
	if err := json.Unmarshal([]byte(items.Value(0)), &obj); err != nil || obj["text"] != "日本語" {
		t.Errorf("nested JSON = %s (%v)", items.Value(0), err)
	}
}
