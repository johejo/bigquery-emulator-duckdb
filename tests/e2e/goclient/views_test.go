package goclient

import (
	"context"
	"errors"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/googleapi"
)

func TestViewLifecycle(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_views")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	view := dataset.Table("v")
	definition := "SELECT 7 AS id, STRUCT('x' AS name, [1, 2] AS tags) AS nested"
	if err := view.Create(ctx, &bigquery.TableMetadata{ViewQuery: definition}); err != nil {
		t.Fatal(err)
	}
	metadata, err := view.Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if metadata.Type != bigquery.ViewTable || metadata.ViewQuery != definition || metadata.UseLegacySQL {
		t.Fatalf("unexpected view metadata: %+v", metadata)
	}
	if len(metadata.Schema) != 2 || metadata.Schema[0].Type != bigquery.IntegerFieldType ||
		len(metadata.Schema[1].Schema) != 2 || !metadata.Schema[1].Schema[1].Repeated {
		t.Fatalf("unexpected view schema: %+v", metadata.Schema)
	}
	// Duplicate creates must preserve the definition and report the HTTP conflict.
	if err := view.Create(ctx, &bigquery.TableMetadata{ViewQuery: "SELECT 9 AS changed"}); err == nil {
		t.Fatal("duplicate view creation succeeded")
	} else {
		var apiErr *googleapi.Error
		if !errors.As(err, &apiErr) || apiErr.Code != 409 {
			t.Fatalf("expected conflict, got %v", err)
		}
	}
	rows, err := client.Query("SELECT id, nested.name AS name FROM go_views.v").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var row struct {
		ID   int64
		Name string
	}
	if err := rows.Next(&row); err != nil || row.ID != 7 || row.Name != "x" {
		t.Fatalf("view row = %+v, error = %v", row, err)
	}
	if err := view.Delete(ctx); err != nil {
		t.Fatal(err)
	}
	if _, err := view.Metadata(ctx); err == nil {
		t.Fatal("deleted view still exists")
	}
}
