package storage

import (
	"bytes"
	"encoding/json"
	"testing"

	"cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"github.com/apache/arrow-go/v18/arrow/array"
	"github.com/apache/arrow-go/v18/arrow/ipc"
)

// The Storage Read type table specifies Utf8 for JSON, distinct from a NULL slot.
// third_party/bigquery_docs/reference/storage.md#arrow-schema-details
func TestReadArrowJSON(t *testing.T) {
	t.Parallel()
	rest, read, _ := clients(t)
	path := table(t, rest, "storage_read_arrow_json", "sql_null JSON, json_null JSON, nested STRUCT<items ARRAY<JSON>>")
	query(t, rest, `INSERT INTO storage_read_arrow_json.rows VALUES
		(NULL, JSON 'null', STRUCT([JSON '{"text":"日本語"}'] AS items))`)
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
	items := batch.Column(2).(*array.Struct).Field(0).(*array.List).ListValues().(*array.String)
	var obj map[string]string
	if err := json.Unmarshal([]byte(items.Value(0)), &obj); err != nil || obj["text"] != "日本語" {
		t.Errorf("nested JSON = %s (%v)", items.Value(0), err)
	}
}
