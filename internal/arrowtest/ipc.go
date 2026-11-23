// Package arrowtest builds Arrow IPC request data for Storage Write tests and probes.
package arrowtest

import (
	"bytes"
	"encoding/binary"

	"github.com/apache/arrow-go/v18/arrow"
	"github.com/apache/arrow-go/v18/arrow/ipc"
)

// Encode returns the schema and batch messages separately, without a stream terminator.
// The record must not contain dictionary arrays.
func Encode(record arrow.RecordBatch) (schema, batch []byte, err error) {
	var output bytes.Buffer
	w := ipc.NewWriter(&output, ipc.WithSchema(record.Schema()))
	if err := w.Write(record); err != nil {
		return nil, nil, err
	}
	end := output.Len()
	if err := w.Close(); err != nil {
		return nil, nil, err
	}
	wire := output.Bytes()
	boundary := 8 + int(binary.LittleEndian.Uint32(wire[4:8]))
	return wire[:boundary:boundary], wire[boundary:end:end], nil
}
