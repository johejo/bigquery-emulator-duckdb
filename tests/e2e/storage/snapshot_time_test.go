package storage

import (
	"slices"
	"testing"
	"time"

	"cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"google.golang.org/protobuf/types/known/timestamppb"
)

// A Storage Read session can select a snapshot from before its creation.
// third_party/bigquery_docs/reference/storage.md
func TestReadHistoricalSnapshot(t *testing.T) {
	t.Parallel()
	rest, read, _ := clients(t)
	path := table(t, rest, "storage_read_history", "id INT64, keep BOOL")
	query(t, rest, "INSERT INTO storage_read_history.rows VALUES (1, TRUE), (2, FALSE)")
	before := time.Now().Truncate(time.Microsecond)
	query(t, rest, "UPDATE storage_read_history.rows SET id = 3 WHERE keep")
	options := &storagepb.ReadSession_TableReadOptions{SelectedFields: []string{"id"}, RowRestriction: "keep"}
	for _, format := range []storagepb.DataFormat{storagepb.DataFormat_ARROW, storagepb.DataFormat_AVRO} {
		t.Run(format.String(), func(t *testing.T) {
			s, err := read.CreateReadSession(t.Context(), &storagepb.CreateReadSessionRequest{
				Parent: "projects/test", MaxStreamCount: 1,
				ReadSession: &storagepb.ReadSession{
					Table: path, DataFormat: format, ReadOptions: options,
					TableModifiers: &storagepb.ReadSession_TableModifiers{SnapshotTime: timestamppb.New(before)},
				},
			})
			if err != nil {
				t.Fatal(err)
			}
			if got := readIDs(t, read, s, s.Streams[0].Name, 0); !slices.Equal(got, []int64{1}) {
				t.Errorf("historical rows = %v, want [1]", got)
			}
			current := session(t, read, path, format, options, 1)
			if got := readIDs(t, read, current, current.Streams[0].Name, 0); !slices.Equal(got, []int64{3}) {
				t.Errorf("current rows = %v, want [3]", got)
			}
		})
	}
}
