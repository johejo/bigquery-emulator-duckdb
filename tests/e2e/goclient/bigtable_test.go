package goclient

import (
	"bytes"
	"fmt"
	"os"
	"reflect"
	"strings"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	adminpb "cloud.google.com/go/bigtable/admin/apiv2/adminpb"
	btpb "cloud.google.com/go/bigtable/apiv2/bigtablepb"
	"google.golang.org/api/iterator"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

// BigtableOptions and the BIGTABLE format are specified in
// third_party/bigquery_docs/reference/rest/v2/tables.md; the DDL URI and
// CLOUD_BIGTABLE spelling in third_party/bigquery_docs/reference/standard-sql/data-definition-language.md.
func TestBigtableExternal(t *testing.T) {
	t.Parallel()
	client := newClient(t)
	host := os.Getenv("BIGTABLE_EMULATOR_HOST")
	if host == "" {
		t.Fatal("BIGTABLE_EMULATOR_HOST is required; run just e2e")
	}
	ctx := t.Context()
	conn, err := grpc.NewClient(host, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	admin := adminpb.NewBigtableTableAdminClient(conn)
	parent := "projects/test/instances/test"
	tableID := "go-external-bigtable"
	source := parent + "/tables/" + tableID
	_, err = admin.CreateTable(ctx, &adminpb.CreateTableRequest{
		Parent: parent, TableId: tableID, Table: &adminpb.Table{ColumnFamilies: map[string]*adminpb.ColumnFamily{"family": {}}},
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _, _ = admin.DeleteTable(ctx, &adminpb.DeleteTableRequest{Name: source}) })
	data := btpb.NewBigtableClient(conn)
	put := func(key string) {
		t.Helper()
		_, err := data.MutateRow(ctx, &btpb.MutateRowRequest{TableName: source, RowKey: []byte(key), Mutations: []*btpb.Mutation{
			{Mutation: &btpb.Mutation_SetCell_{SetCell: &btpb.Mutation_SetCell{FamilyName: "family", ColumnQualifier: []byte("qualifier"), TimestampMicros: 1000, Value: []byte{0, 0xff, '\'', '\\'}}}},
			{Mutation: &btpb.Mutation_SetCell_{SetCell: &btpb.Mutation_SetCell{FamilyName: "family", ColumnQualifier: []byte("qualifier"), TimestampMicros: 2000, Value: []byte("world")}}},
		}})
		if err != nil {
			t.Fatal(err)
		}
	}
	dataset := client.Dataset("go_bigtable_external")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	uri := "https://googleapis.com/bigtable/" + source
	table := dataset.Table("raw")
	if err := table.Create(ctx, &bigquery.TableMetadata{ExternalDataConfig: &bigquery.ExternalDataConfig{
		SourceFormat: bigquery.Bigtable, SourceURIs: []string{uri},
	}}); err != nil {
		t.Fatal(err)
	}
	metadata, err := table.Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if metadata.Type != bigquery.ExternalTable || metadata.Schema[0].Name != "rowkey" || metadata.Schema[0].Type != bigquery.BytesFieldType {
		t.Fatalf("external table metadata: %+v", metadata)
	}
	config := metadata.ExternalDataConfig
	if config.SourceFormat != bigquery.Bigtable || !reflect.DeepEqual(config.SourceURIs, []string{uri}) {
		t.Fatalf("external config: %+v", config)
	}
	readKeys := func(want ...string) {
		t.Helper()
		q := client.Query("SELECT rowkey FROM go_bigtable_external.raw ORDER BY rowkey")
		q.DisableQueryCache = true
		it, err := q.Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		for _, key := range want {
			var row []bigquery.Value
			if err := it.Next(&row); err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(row[0].([]byte), []byte(key)) {
				t.Fatalf("row = %v, want %q", row, key)
			}
		}
		var extra []bigquery.Value
		if err := it.Next(&extra); err != iterator.Done {
			t.Fatalf("extra row: %v, %v", extra, err)
		}
	}
	readKeys()
	put("first")
	readKeys("first")
	put("second")
	readKeys("first", "second")
	rawValues, err := client.Query("SELECT c.value FROM go_bigtable_external.raw, UNNEST(family.column) col, UNNEST(col.cell) c WHERE rowkey = b'first' ORDER BY c.timestamp").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range [][]byte{{0, 0xff, '\'', '\\'}, []byte("world")} {
		var row []bigquery.Value
		if err := rawValues.Next(&row); err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(row[0].([]byte), want) {
			t.Fatalf("cell = %v, want %q", row, want)
		}
	}
	var extraRaw []bigquery.Value
	if err := rawValues.Next(&extraRaw); err != iterator.Done {
		t.Fatalf("extra raw cell: %v, %v", extraRaw, err)
	}
	profileURI := strings.Replace(uri, "/tables/", "/appProfiles/default/tables/", 1)
	ddl := fmt.Sprintf(`CREATE EXTERNAL TABLE go_bigtable_external.text OPTIONS (
  format = 'CLOUD_BIGTABLE', uris = '%s',
  bigtable_options = '{"readRowkeyAsString":true,"ignoreUnspecifiedColumnFamilies":true,"columnFamilies":[{"familyId":"family","type":"STRING","onlyReadLatest":true}]}')`, profileURI)
	if err := wait(ctx, client.Query(ddl)); err != nil {
		t.Fatal(err)
	}
	textMeta, err := dataset.Table("text").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if textMeta.Schema[0].Type != bigquery.StringFieldType {
		t.Fatalf("rowkey type = %s", textMeta.Schema[0].Type)
	}
	if !reflect.DeepEqual(textMeta.ExternalDataConfig.SourceURIs, []string{profileURI}) {
		t.Fatalf("profile URI = %v", textMeta.ExternalDataConfig.SourceURIs)
	}
	options := textMeta.ExternalDataConfig.Options.(*bigquery.BigtableOptions)
	if !options.ReadRowkeyAsString || !options.IgnoreUnspecifiedColumnFamilies || !options.ColumnFamilies[0].OnlyReadLatest {
		t.Fatalf("options = %+v", options)
	}
	latest, err := client.Query("SELECT rowkey, col.cell.value, col.cell.timestamp FROM go_bigtable_external.text, UNNEST(family.column) col ORDER BY rowkey").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	for _, key := range []string{"first", "second"} {
		var row []bigquery.Value
		if err := latest.Next(&row); err != nil {
			t.Fatal(err)
		}
		if row[0] != key || row[1] != "world" || !row[2].(time.Time).Equal(time.UnixMicro(2000)) {
			t.Fatalf("latest row = %v", row)
		}
	}
	var extra []bigquery.Value
	if err := latest.Next(&extra); err != iterator.Done {
		t.Fatalf("extra latest row: %v, %v", extra, err)
	}
	for _, options := range []string{
		`{"outputColumnFamiliesAsJson":true}`,
		`{"columnFamilies":[{"familyId":"family","type":"INTEGER"}]}`,
		`{"columnFamilies":[{"familyId":"family","columns":[{"qualifierString":"qualifier"}]}]}`,
	} {
		query := fmt.Sprintf(`CREATE EXTERNAL TABLE go_bigtable_external.rejected OPTIONS (format='CLOUD_BIGTABLE', uris='%s', bigtable_options='%s')`, uri, options)
		if err := wait(ctx, client.Query(query)); err == nil || !strings.Contains(err.Error(), "does not support") {
			t.Fatalf("unsupported options %s: %v", options, err)
		}
	}
}
