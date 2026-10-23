package goclient

import (
	"testing"

	"cloud.google.com/go/bigquery"
)

// BigQuery's recorded answers to labels.api.json: REST patches sort existing labels,
// including when changing only the description, then append new labels in key order.
func TestTableLabelOrderAfterPatch(t *testing.T) {
	t.Parallel()
	ctx := t.Context()
	client := newClient(t)
	dataset := client.Dataset("go_label_order_patch")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	if _, _, err := runScript(t, client, dataset.DatasetID,
		`CREATE TABLE t (x INT64) OPTIONS (labels = [('org', 'dev'), ('a', 'b')])`); err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		name        string
		description string
		labels      map[string]string
		remove      string
		want        string
	}{
		{name: "description", description: "updated", want: "[STRUCT(\"a\", \"b\"), STRUCT(\"org\", \"dev\")]"},
		{name: "existing_label", labels: map[string]string{"org": "prod"}, want: "[STRUCT(\"a\", \"b\"), STRUCT(\"org\", \"prod\")]"},
		{name: "new_labels", labels: map[string]string{"b": "middle", "z": "last"}, want: "[STRUCT(\"a\", \"b\"), STRUCT(\"org\", \"prod\"), STRUCT(\"b\", \"middle\"), STRUCT(\"z\", \"last\")]"},
		{name: "delete_label", remove: "a", want: "[STRUCT(\"b\", \"middle\"), STRUCT(\"org\", \"prod\"), STRUCT(\"z\", \"last\")]"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var update bigquery.TableMetadataToUpdate
			if tc.description != "" {
				update.Description = tc.description
			}
			for key, value := range tc.labels {
				update.SetLabel(key, value)
			}
			if tc.remove != "" {
				update.DeleteLabel(tc.remove)
			}
			if _, err := dataset.Table("t").Update(ctx, update, ""); err != nil {
				t.Fatal(err)
			}
			query := client.Query("SELECT option_value FROM " + dataset.DatasetID + ".INFORMATION_SCHEMA.TABLE_OPTIONS WHERE table_name = 't' AND option_name = 'labels'")
			rows, err := query.Read(ctx)
			if err != nil {
				t.Fatal(err)
			}
			var got struct {
				Value string `bigquery:"option_value"`
			}
			if err := rows.Next(&got); err != nil {
				t.Fatal(err)
			}
			if got.Value != tc.want {
				t.Errorf("label order = %q, want %q", got.Value, tc.want)
			}
		})
	}
}
