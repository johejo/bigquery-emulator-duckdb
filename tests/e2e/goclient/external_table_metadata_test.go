package goclient

import (
	"reflect"
	"strings"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
)

// tables.patch updates an external table's description, friendly name, labels, tags and
// expiration time and keeps its definition, as
// [tables.patch](third_party/bigquery_docs/reference/rest/v2/tables.md) does for any table. The
// table's source does not exist; creating it with a schema and updating it do not read it.
func TestExternalTableMetadataPatch(t *testing.T) {
	t.Parallel()
	ctx := t.Context()
	client := newClient(t)
	dataset := newDataset(t, client, "go_external_table_metadata")
	table := dataset.Table("ext")
	config := &bigquery.ExternalDataConfig{
		SourceFormat: bigquery.Parquet,
		SourceURIs:   []string{"gs://go-external-table-metadata-missing/data.parquet"},
	}
	schema := bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}}
	if err := table.Create(ctx, &bigquery.TableMetadata{
		Schema:             schema,
		ExternalDataConfig: config,
		Labels:             map[string]string{"env": "dev"},
	}); err != nil {
		t.Fatal(err)
	}

	expiration := time.Now().Add(24 * time.Hour).Truncate(time.Millisecond)
	var update bigquery.TableMetadataToUpdate
	update.Description = "patched"
	update.Name = "External"
	update.ExpirationTime = expiration
	update.ResourceTags = map[string]string{"123456789012/env": "prod"}
	update.SetLabel("tier", "1")
	update.DeleteLabel("env")
	got, err := table.Update(ctx, update, "")
	if err != nil {
		t.Fatal(err)
	}
	if got.Description != "patched" || got.Name != "External" ||
		!reflect.DeepEqual(got.Labels, map[string]string{"tier": "1"}) ||
		!got.ExpirationTime.Equal(expiration) ||
		!reflect.DeepEqual(got.ResourceTags, map[string]string{"123456789012/env": "prod"}) {
		t.Fatalf("patched metadata = %+v", got)
	}
	got, err = table.Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if got.Type != bigquery.ExternalTable || got.Description != "patched" ||
		got.ExternalDataConfig == nil ||
		got.ExternalDataConfig.SourceFormat != config.SourceFormat ||
		!reflect.DeepEqual(got.ExternalDataConfig.SourceURIs, config.SourceURIs) ||
		len(got.Schema) != 1 || got.Schema[0].Name != "id" {
		t.Fatalf("metadata after patch = %+v", got)
	}

	// A client may send back the definition it read; the unchanged schema and configuration are
	// kept.
	var resend bigquery.TableMetadataToUpdate
	resend.Description = "resent"
	resend.Schema = got.Schema
	resend.ExternalDataConfig = got.ExternalDataConfig
	if got, err = table.Update(ctx, resend, ""); err != nil {
		t.Fatal(err)
	} else if got.Description != "resent" ||
		!reflect.DeepEqual(got.ExternalDataConfig.SourceURIs, config.SourceURIs) {
		t.Fatalf("metadata after resending the definition = %+v", got)
	}

	// Clearing the expiration time leaves the table without one.
	var clear bigquery.TableMetadataToUpdate
	clear.ExpirationTime = bigquery.NeverExpire
	if got, err = table.Update(ctx, clear, ""); err != nil {
		t.Fatal(err)
	} else if !got.ExpirationTime.IsZero() {
		t.Fatalf("expiration after clearing = %v", got.ExpirationTime)
	}

	// The emulator rejects updates that change how the table is read, and options external tables
	// do not take, rather than storing them.
	for name, update := range map[string]bigquery.TableMetadataToUpdate{
		"schema": {Schema: append(bigquery.Schema{}, schema[0],
			&bigquery.FieldSchema{Name: "name", Type: bigquery.StringFieldType})},
		"external_data_config": {ExternalDataConfig: &bigquery.ExternalDataConfig{
			SourceFormat: bigquery.Parquet,
			SourceURIs:   []string{"gs://go-external-table-metadata-missing/other.parquet"},
		}},
		"clustering":               {Clustering: &bigquery.Clustering{Fields: []string{"id"}}},
		"require_partition_filter": {RequirePartitionFilter: true},
	} {
		if _, err := table.Update(ctx, update, ""); err == nil ||
			!strings.Contains(err.Error(), "The emulator does not support") {
			t.Errorf("%s: err = %v, want unsupported", name, err)
		}
	}
	if got, err = table.Metadata(ctx); err != nil {
		t.Fatal(err)
	} else if len(got.Schema) != 1 || got.Clustering != nil ||
		!reflect.DeepEqual(got.ExternalDataConfig.SourceURIs, config.SourceURIs) {
		t.Fatalf("metadata after rejected updates = %+v", got)
	}
}

// BigQuery's DDL reference says ALTER TABLE SET OPTIONS "is not supported for external tables"
// (third_party/bigquery_docs/reference/standard-sql/data-definition-language.md); its message is
// not known, so the emulator rejects the statement as unsupported. external_table_alter.api.yaml
// in testdata/unverified/ records the case for BigQuery's answer.
func TestExternalTableAlterSetOptions(t *testing.T) {
	t.Parallel()
	client := newClient(t)
	dataset := newDataset(t, client, "go_external_table_alter")
	queries := datasetQueries{t, client, dataset}
	queries.mustRun(`CREATE EXTERNAL TABLE ext (id INT64)
		OPTIONS (format = 'PARQUET', uris = ['gs://go-external-table-alter-missing/data.parquet'])`)
	for _, sql := range []string{
		"ALTER TABLE ext SET OPTIONS (description = 'altered')",
		"ALTER TABLE ext SET OPTIONS (labels = [('env', 'dev')])",
		"ALTER TABLE ext SET OPTIONS (expiration_timestamp = TIMESTAMP '2100-01-01 00:00:00 UTC')",
	} {
		if err := queries.run(sql); err == nil ||
			!strings.Contains(err.Error(), "The emulator does not support ALTER TABLE on external tables") {
			t.Errorf("%s: err = %v, want unsupported", sql, err)
		}
	}
	got, err := dataset.Table("ext").Metadata(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	if got.Description != "" || len(got.Labels) != 0 || !got.ExpirationTime.IsZero() {
		t.Fatalf("metadata after rejected ALTER = %+v", got)
	}
}
