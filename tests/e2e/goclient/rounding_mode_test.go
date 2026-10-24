package goclient

import (
	"context"
	"errors"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

// SQL/API rounding semantics and all load/copy boundaries are in rounding_modes.api.yaml.
// Keep the SDK's conversion of insertAll row errors to PutMultiError in Go.
func TestRoundingInsertErrorsDecodeAsPutMultiError(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_rounding_sdk_errors")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	table := dataset.Table("t")
	schema := bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}, {Name: "even", Type: bigquery.NumericFieldType, Precision: 5, Scale: 1, RoundingMode: bigquery.RoundHalfEven}}
	if err := table.Create(ctx, &bigquery.TableMetadata{Schema: schema}); err != nil {
		t.Fatal(err)
	}
	err := table.Inserter().Put(ctx, jsonRow{"id": 8, "even": 9999.95})
	const outOfRange = "NUMERIC(5, 1) has precision 5 and scale 1 but got a value that is not in range of [-9999.9, 9999.9]"
	var rowErrors bigquery.PutMultiError
	if !errors.As(err, &rowErrors) || len(rowErrors) != 1 || !strings.Contains(rowErrors[0].Error(), outOfRange) {
		t.Fatalf("insertAll error = %v, want one PutMultiError row containing %q", err, outOfRange)
	}
}
