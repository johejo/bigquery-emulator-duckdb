package goclient

import (
	"bytes"
	"context"
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"reflect"
	"slices"
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
		Parent: parent, TableId: tableID,
		Table: &adminpb.Table{ColumnFamilies: map[string]*adminpb.ColumnFamily{"family": {}, "other_family": {}, "integers": {}, "decoded": {}, "texts": {}, "jsons": {}}},
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _, _ = admin.DeleteTable(ctx, &adminpb.DeleteTableRequest{Name: source}) })
	data := btpb.NewBigtableClient(conn)
	put := func(key, family string) {
		t.Helper()
		_, err := data.MutateRow(ctx, &btpb.MutateRowRequest{TableName: source, RowKey: []byte(key), Mutations: []*btpb.Mutation{
			{Mutation: &btpb.Mutation_SetCell_{SetCell: &btpb.Mutation_SetCell{FamilyName: family, ColumnQualifier: []byte("qualifier"), TimestampMicros: 1000, Value: []byte{0, 0xff, '\'', '\\'}}}},
			{Mutation: &btpb.Mutation_SetCell_{SetCell: &btpb.Mutation_SetCell{FamilyName: family, ColumnQualifier: []byte("qualifier"), TimestampMicros: 2000, Value: []byte("world")}}},
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
	put("first", "family")
	readKeys("first")
	put("second", "family")
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
	// Verified on BigQuery (run nedc75o5ucmak3pzj56nlvwqin).
	// Do not UNNEST: that could also discard missing families.
	put("other", "other_family")
	latest, err := client.Query(`SELECT rowkey, family.column[SAFE_OFFSET(0)].cell.value,
		family.column[SAFE_OFFSET(0)].cell.timestamp FROM go_bigtable_external.text ORDER BY rowkey`).Read(ctx)
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

	if err := dataset.Table("both").Create(ctx, &bigquery.TableMetadata{ExternalDataConfig: &bigquery.ExternalDataConfig{
		SourceFormat: bigquery.Bigtable,
		SourceURIs:   []string{uri},
		Options: &bigquery.BigtableOptions{
			ReadRowkeyAsString:              true,
			IgnoreUnspecifiedColumnFamilies: true,
			ColumnFamilies: []*bigquery.BigtableColumnFamily{
				{FamilyID: "family", Type: "STRING", OnlyReadLatest: true},
				{FamilyID: "other_family", Type: "STRING", OnlyReadLatest: true},
			},
		},
	}}); err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		query string
		rows  [][]bigquery.Value
	}{
		{"SELECT rowkey FROM go_bigtable_external.text ORDER BY rowkey",
			[][]bigquery.Value{{"first"}, {"other"}, {"second"}}},
		{"SELECT rowkey, family IS NULL FROM go_bigtable_external.both ORDER BY rowkey",
			[][]bigquery.Value{{"first", false}, {"second", false}}},
		{"SELECT rowkey, family IS NULL, other_family IS NULL FROM go_bigtable_external.both ORDER BY rowkey",
			[][]bigquery.Value{{"first", false, true}, {"other", true, false}, {"second", false, true}}},
	} {
		it, err := client.Query(tc.query).Read(ctx)
		if err != nil {
			t.Fatalf("%s: %v", tc.query, err)
		}
		for _, want := range tc.rows {
			var row []bigquery.Value
			if err := it.Next(&row); err != nil || !reflect.DeepEqual(row, want) {
				t.Fatalf("%s: row = %v, err = %v, want %v", tc.query, row, err, want)
			}
		}
		if err := it.Next(&extra); err != iterator.Done {
			t.Fatalf("%s: extra row = %v, err = %v", tc.query, extra, err)
		}
	}

	// BigtableColumnFamily and BigtableColumn specify HBase Bytes.toBytes for BINARY;
	// the long overload stores the signed INT64 bits in big-endian order.
	integerCell := func(family, qualifier string, ts, value int64) *btpb.Mutation {
		return &btpb.Mutation{Mutation: &btpb.Mutation_SetCell_{SetCell: &btpb.Mutation_SetCell{
			FamilyName: family, ColumnQualifier: []byte(qualifier), TimestampMicros: ts,
			Value: binary.BigEndian.AppendUint64(nil, uint64(value)),
		}}}
	}
	if _, err := data.MutateRow(ctx, &btpb.MutateRowRequest{TableName: source, RowKey: []byte("numbers"), Mutations: []*btpb.Mutation{
		integerCell("integers", "loose", 1000, math.MinInt64),
		integerCell("integers", "loose", 2000, -42),
		integerCell("integers", "limits", 2000, math.MaxInt64),
		{Mutation: &btpb.Mutation_SetCell_{SetCell: &btpb.Mutation_SetCell{
			FamilyName: "other_family", ColumnQualifier: []byte("number"), TimestampMicros: 2000,
			Value: []byte{0xff, 0xff, 0xff, 0xfe}, // HBase Bytes.toBytes(int) for -2.
		}}},
	}}); err != nil {
		t.Fatal(err)
	}
	numbers := dataset.Table("numbers")
	if err := numbers.Create(ctx, &bigquery.TableMetadata{ExternalDataConfig: &bigquery.ExternalDataConfig{
		SourceFormat: bigquery.Bigtable,
		SourceURIs:   []string{uri},
		Options: &bigquery.BigtableOptions{
			IgnoreUnspecifiedColumnFamilies: true,
			ColumnFamilies: []*bigquery.BigtableColumnFamily{
				{FamilyID: "integers", Type: "INTEGER", Encoding: "BINARY", Columns: []*bigquery.BigtableColumn{
					{Qualifier: "limits", OnlyReadLatest: true},
				}},
				{FamilyID: "other_family", Type: "BYTES", Encoding: "TEXT", Columns: []*bigquery.BigtableColumn{
					{Qualifier: "number", Type: "INTEGER", Encoding: "BINARY"},
				}},
			},
		},
	}}); err != nil {
		t.Fatal(err)
	}
	numMeta, err := numbers.Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	for _, value := range []*bigquery.FieldSchema{
		numMeta.Schema[1].Schema[0].Schema[1].Schema[1], // integers.column.cell.value
		numMeta.Schema[1].Schema[1].Schema[0].Schema[1], // integers.limits.cell.value
		numMeta.Schema[2].Schema[1].Schema[0].Schema[1], // other_family.number.cell.value
	} {
		if value.Name != "value" || value.Type != bigquery.IntegerFieldType {
			t.Fatalf("expected INTEGER value schema, got %+v", value)
		}
	}
	it, err := client.Query(`SELECT integers.column[OFFSET(0)].cell[OFFSET(0)].value AS newest,
		integers.column[OFFSET(0)].cell[OFFSET(1)].value AS oldest,
		integers.limits.cell.value AS max_value, other_family.number.cell[OFFSET(0)].value AS short_value
		FROM go_bigtable_external.numbers`).Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	want := [][]bigquery.Value{{int64(-42), int64(math.MinInt64), int64(math.MaxInt64), int64(-2)}}
	if rows := readRows(t, it); !reflect.DeepEqual(rows, want) {
		t.Fatalf("BINARY INTEGER rows = %v, want %v", rows, want)
	}

	// HBase Bytes.toBytes(double) stores the IEEE 754 bits in big-endian order, and
	// Bytes.toBytes(boolean) stores 0xff for true and 0x00 for false.
	decodedCell := func(qualifier string, ts int64, value []byte) *btpb.Mutation {
		return &btpb.Mutation{Mutation: &btpb.Mutation_SetCell_{SetCell: &btpb.Mutation_SetCell{
			FamilyName: "decoded", ColumnQualifier: []byte(qualifier), TimestampMicros: ts, Value: value,
		}}}
	}
	if _, err := data.MutateRow(ctx, &btpb.MutateRowRequest{TableName: source, RowKey: []byte("decoded"), Mutations: []*btpb.Mutation{
		decodedCell("ratio", 1000, binary.BigEndian.AppendUint64(nil, math.Float64bits(math.Inf(-1)))),
		decodedCell("ratio", 2000, binary.BigEndian.AppendUint64(nil, math.Float64bits(-2.5))),
		decodedCell("flag", 1000, []byte{0x00}),
		decodedCell("flag", 2000, []byte{0xff}),
		decodedCell("loose", 2000, binary.BigEndian.AppendUint64(nil, math.Float64bits(math.Pi))),
	}}); err != nil {
		t.Fatal(err)
	}
	decoded := dataset.Table("floats_booleans")
	if err := decoded.Create(ctx, &bigquery.TableMetadata{ExternalDataConfig: &bigquery.ExternalDataConfig{
		SourceFormat: bigquery.Bigtable,
		SourceURIs:   []string{uri},
		Options: &bigquery.BigtableOptions{
			IgnoreUnspecifiedColumnFamilies: true,
			ColumnFamilies: []*bigquery.BigtableColumnFamily{
				{FamilyID: "decoded", Type: "FLOAT", Encoding: "BINARY", Columns: []*bigquery.BigtableColumn{
					{Qualifier: "ratio"},
					{Qualifier: "flag", Type: "BOOLEAN"},
				}},
			},
		},
	}}); err != nil {
		t.Fatal(err)
	}
	decodedMeta, err := decoded.Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	for _, value := range []struct {
		field *bigquery.FieldSchema
		want  bigquery.FieldType
	}{
		{decodedMeta.Schema[1].Schema[0].Schema[1].Schema[1], bigquery.FloatFieldType},   // decoded.column.cell.value
		{decodedMeta.Schema[1].Schema[1].Schema[0].Schema[1], bigquery.FloatFieldType},   // decoded.ratio.cell.value
		{decodedMeta.Schema[1].Schema[2].Schema[0].Schema[1], bigquery.BooleanFieldType}, // decoded.flag.cell.value
	} {
		if value.field.Name != "value" || value.field.Type != value.want {
			t.Fatalf("expected %s value schema, got %+v", value.want, value.field)
		}
	}
	it, err = client.Query(`SELECT decoded.ratio.cell[OFFSET(0)].value AS newest_ratio,
		decoded.ratio.cell[OFFSET(1)].value AS oldest_ratio,
		decoded.flag.cell[OFFSET(0)].value AS newest_flag, decoded.flag.cell[OFFSET(1)].value AS oldest_flag,
		decoded.column[OFFSET(0)].cell[OFFSET(0)].value AS loose
		FROM go_bigtable_external.floats_booleans`).Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	want = [][]bigquery.Value{{-2.5, math.Inf(-1), true, false, math.Pi}}
	if rows := readRows(t, it); !reflect.DeepEqual(rows, want) {
		t.Fatalf("BINARY FLOAT and BOOLEAN rows = %v, want %v", rows, want)
	}

	// A column's encoding takes precedence over its family's, as BigtableColumn.encoding documents.
	if _, err := data.MutateRow(ctx, &btpb.MutateRowRequest{TableName: source, RowKey: []byte("texts"), Mutations: []*btpb.Mutation{
		{Mutation: &btpb.Mutation_SetCell_{SetCell: &btpb.Mutation_SetCell{
			FamilyName: "texts", ColumnQualifier: []byte("loose"), TimestampMicros: 2000, Value: []byte("-7"),
		}}},
		integerCell("texts", "binary", 2000, 7),
	}}); err != nil {
		t.Fatal(err)
	}
	texts := dataset.Table("text_cells")
	if err := texts.Create(ctx, &bigquery.TableMetadata{ExternalDataConfig: &bigquery.ExternalDataConfig{
		SourceFormat: bigquery.Bigtable,
		SourceURIs:   []string{uri},
		Options: &bigquery.BigtableOptions{
			IgnoreUnspecifiedColumnFamilies: true,
			ColumnFamilies: []*bigquery.BigtableColumnFamily{
				{FamilyID: "texts", Type: "INTEGER", Encoding: "TEXT", Columns: []*bigquery.BigtableColumn{
					{Qualifier: "binary", Encoding: "BINARY", OnlyReadLatest: true},
				}},
			},
		},
	}}); err != nil {
		t.Fatal(err)
	}
	it, err = client.Query(`SELECT texts.column[OFFSET(0)].cell[OFFSET(0)].value AS loose, texts.binary.cell.value AS ` + "`binary`" + `
		FROM go_bigtable_external.text_cells`).Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	want = [][]bigquery.Value{{int64(-7), int64(7)}}
	if rows := readRows(t, it); !reflect.DeepEqual(rows, want) {
		t.Fatalf("TEXT and BINARY INTEGER rows = %v, want %v", rows, want)
	}

	// JSON cells read as compact text whatever their encoding, as bigtable_json.api.yaml verifies.
	if _, err := data.MutateRow(ctx, &btpb.MutateRowRequest{TableName: source, RowKey: []byte("jsons"), Mutations: []*btpb.Mutation{
		{Mutation: &btpb.Mutation_SetCell_{SetCell: &btpb.Mutation_SetCell{
			FamilyName: "jsons", ColumnQualifier: []byte("loose"), TimestampMicros: 1000, Value: []byte(`{"b": 1, "a": 2}`),
		}}},
		{Mutation: &btpb.Mutation_SetCell_{SetCell: &btpb.Mutation_SetCell{
			FamilyName: "jsons", ColumnQualifier: []byte("loose"), TimestampMicros: 2000, Value: []byte(`null`),
		}}},
		{Mutation: &btpb.Mutation_SetCell_{SetCell: &btpb.Mutation_SetCell{
			FamilyName: "jsons", ColumnQualifier: []byte("named"), TimestampMicros: 2000, Value: []byte(` [1, "x"] `),
		}}},
	}}); err != nil {
		t.Fatal(err)
	}
	jsons := dataset.Table("json_cells")
	if err := jsons.Create(ctx, &bigquery.TableMetadata{ExternalDataConfig: &bigquery.ExternalDataConfig{
		SourceFormat: bigquery.Bigtable,
		SourceURIs:   []string{uri},
		Options: &bigquery.BigtableOptions{
			IgnoreUnspecifiedColumnFamilies: true,
			ColumnFamilies: []*bigquery.BigtableColumnFamily{
				{FamilyID: "jsons", Type: "JSON", Columns: []*bigquery.BigtableColumn{
					{Qualifier: "named", Encoding: "BINARY", OnlyReadLatest: true},
				}},
			},
		},
	}}); err != nil {
		t.Fatal(err)
	}
	it, err = client.Query(`SELECT jsons.column[OFFSET(0)].cell[OFFSET(0)].value AS newest,
		jsons.column[OFFSET(0)].cell[OFFSET(0)].value IS NULL AS newest_is_null,
		jsons.column[OFFSET(0)].cell[OFFSET(1)].value AS oldest, jsons.named.cell.value AS named
		FROM go_bigtable_external.json_cells`).Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	want = [][]bigquery.Value{{"null", false, `{"a":2,"b":1}`, `[1,"x"]`}}
	if rows := readRows(t, it); !reflect.DeepEqual(rows, want) {
		t.Fatalf("JSON rows = %v, want %v", rows, want)
	}
}

// Row key filters keep the rows that BigQuery's comparison operators, IN, BETWEEN and
// STARTS_WITH keep, which order BYTES and STRING values by their bytes, as Bigtable orders row
// keys (third_party/bigquery_docs/reference/standard-sql/operators.md). Filters do not reach
// a scan through LIMIT, an outer join or a WITH query referenced twice.
func TestBigtableRowKeyFilters(t *testing.T) {
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
	// Subtests run in parallel after this function returns; cleanups run after them.
	t.Cleanup(func() { _ = conn.Close() })
	admin := adminpb.NewBigtableTableAdminClient(conn)
	data := btpb.NewBigtableClient(conn)
	parent := "projects/test/instances/test"
	// Creates a Bigtable table of rows with `keys` and returns its URI.
	create := func(tableID string, keys []string) string {
		t.Helper()
		source := parent + "/tables/" + tableID
		if _, err := admin.CreateTable(ctx, &adminpb.CreateTableRequest{
			Parent: parent, TableId: tableID,
			Table: &adminpb.Table{ColumnFamilies: map[string]*adminpb.ColumnFamily{"family": {}}},
		}); err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() {
			_, _ = admin.DeleteTable(context.Background(), &adminpb.DeleteTableRequest{Name: source})
		})
		for _, key := range keys {
			if _, err := data.MutateRow(ctx, &btpb.MutateRowRequest{TableName: source, RowKey: []byte(key), Mutations: []*btpb.Mutation{
				{Mutation: &btpb.Mutation_SetCell_{SetCell: &btpb.Mutation_SetCell{FamilyName: "family", ColumnQualifier: []byte("q"), TimestampMicros: 1000, Value: []byte("v")}}},
			}}); err != nil {
				t.Fatal(err)
			}
		}
		return "https://googleapis.com/bigtable/" + source
	}
	keys := []string{"a", "ab", "abc", "b", "b\xff", "b\xff\xff", "c", "z", "\xc3\xa9", "\xff", "\xff\xff"}
	raw := create("go-bigtable-row-key-filters", keys)
	// Every key of the STRING table is valid UTF-8.
	text := create("go-bigtable-row-key-filters-text", []string{"a", "c", "z", "\xc3\xa9", "\xe6\x97\xa5"})
	newDataset(t, client, "go_bigtable_row_key_filters")
	for _, ddl := range []string{
		fmt.Sprintf(`CREATE EXTERNAL TABLE go_bigtable_row_key_filters.raw OPTIONS (format = 'CLOUD_BIGTABLE', uris = '%s')`, raw),
		fmt.Sprintf(`CREATE EXTERNAL TABLE go_bigtable_row_key_filters.text OPTIONS (format = 'CLOUD_BIGTABLE', uris = '%s',
  bigtable_options = '{"readRowkeyAsString":true}')`, text),
	} {
		if err := wait(ctx, client.Query(ddl)); err != nil {
			t.Fatal(err)
		}
	}
	for _, test := range []struct {
		name       string
		sql        string
		parameters []bigquery.QueryParameter
		want       []string
	}{
		{name: "equal", sql: `SELECT rowkey FROM raw WHERE rowkey = b'ab'`, want: []string{"ab"}},
		{name: "missing", sql: `SELECT rowkey FROM raw WHERE rowkey = b'aa'`},
		{name: "range", sql: `SELECT rowkey FROM raw WHERE rowkey > b'ab' AND rowkey <= b'b'`, want: []string{"abc", "b"}},
		{name: "constant first", sql: `SELECT rowkey FROM raw WHERE b'b' > rowkey`, want: []string{"a", "ab", "abc"}},
		{name: "at least", sql: `SELECT rowkey FROM raw WHERE rowkey >= b'z'`, want: []string{"z", "\xc3\xa9", "\xff", "\xff\xff"}},
		{name: "between", sql: `SELECT rowkey FROM raw WHERE rowkey BETWEEN b'ab' AND b'b'`, want: []string{"ab", "abc", "b"}},
		{name: "empty between", sql: `SELECT rowkey FROM raw WHERE rowkey BETWEEN b'b' AND b'a'`},
		{name: "in", sql: `SELECT rowkey FROM raw WHERE rowkey IN (b'c', b'a', NULL, b'zz')`, want: []string{"a", "c"}},
		{name: "prefix", sql: `SELECT rowkey FROM raw WHERE STARTS_WITH(rowkey, b'ab')`, want: []string{"ab", "abc"}},
		{name: "prefix ending in 0xff", sql: `SELECT rowkey FROM raw WHERE STARTS_WITH(rowkey, b'b\xff')`, want: []string{"b\xff", "b\xff\xff"}},
		{name: "prefix of 0xff", sql: `SELECT rowkey FROM raw WHERE STARTS_WITH(rowkey, b'\xff')`, want: []string{"\xff", "\xff\xff"}},
		{name: "empty prefix", sql: `SELECT rowkey FROM raw WHERE STARTS_WITH(rowkey, b'') AND rowkey < b'ab'`, want: []string{"a"}},
		{name: "or", sql: `SELECT rowkey FROM raw WHERE rowkey = b'a' OR rowkey > b'z'`, want: []string{"a", "\xc3\xa9", "\xff", "\xff\xff"}},
		{name: "or with another column", sql: `SELECT rowkey FROM raw WHERE rowkey = b'a' OR ARRAY_LENGTH(family.column) = 0`, want: []string{"a"}},
		{name: "disjoint", sql: `SELECT rowkey FROM raw WHERE rowkey < b'b' AND rowkey > b'b'`},
		{name: "below empty", sql: `SELECT rowkey FROM raw WHERE rowkey <= b''`},
		{name: "above empty", sql: `SELECT COUNT(*) FROM raw WHERE rowkey > b''`, want: []string{fmt.Sprint(len(keys))}},
		{name: "where false", sql: `SELECT rowkey FROM raw WHERE FALSE AND rowkey = b'a'`},
		{name: "named parameter", sql: `SELECT rowkey FROM raw WHERE rowkey = @key`, parameters: []bigquery.QueryParameter{{Name: "key", Value: []byte("abc")}}, want: []string{"abc"}},
		{name: "positional parameter", sql: `SELECT rowkey FROM raw WHERE rowkey < ?`, parameters: []bigquery.QueryParameter{{Value: []byte("ab")}}, want: []string{"a"}},
		{name: "script variable", sql: `DECLARE key BYTES DEFAULT b'c'; SELECT rowkey FROM raw WHERE rowkey >= key AND rowkey < b'zz'`, want: []string{"c", "z"}},
		{name: "unnest", sql: `SELECT rowkey FROM raw, UNNEST(family.column) AS col WHERE rowkey = b'b'`, want: []string{"b"}},
		{name: "inner join", sql: `SELECT x.rowkey FROM raw AS x JOIN raw AS y ON x.rowkey = y.rowkey AND y.rowkey = b'c'`, want: []string{"c"}},
		{name: "subquery", sql: `SELECT rowkey FROM (SELECT rowkey FROM raw) WHERE rowkey = b'b'`, want: []string{"b"}},
		{name: "limit", sql: `SELECT rowkey FROM (SELECT rowkey FROM raw ORDER BY rowkey LIMIT 2) WHERE rowkey >= b'ab'`, want: []string{"ab"}},
		{name: "outer join", sql: `SELECT x.rowkey FROM raw AS x LEFT JOIN raw AS y ON x.rowkey = b'a' AND y.rowkey = b'b' WHERE x.rowkey < b'b'`, want: []string{"a", "ab", "abc"}},
		{name: "aggregate", sql: `SELECT rowkey FROM (SELECT rowkey, COUNT(*) AS n FROM raw GROUP BY rowkey) WHERE rowkey = b'c'`, want: []string{"c"}},
		{name: "with", sql: `WITH k AS (SELECT rowkey FROM raw) SELECT rowkey FROM k WHERE rowkey = b'a' UNION ALL SELECT rowkey FROM k WHERE rowkey = b'b'`, want: []string{"a", "b"}},
		{name: "string", sql: `SELECT CAST(rowkey AS BYTES) FROM text WHERE rowkey > 'z' AND rowkey < '日'`, want: []string{"\xc3\xa9"}},
		{name: "string prefix", sql: `SELECT CAST(rowkey AS BYTES) FROM text WHERE STARTS_WITH(rowkey, 'é')`, want: []string{"\xc3\xa9"}},
		{name: "string parameter", sql: `SELECT CAST(rowkey AS BYTES) FROM text WHERE rowkey IN (@key, 'c')`, parameters: []bigquery.QueryParameter{{Name: "key", Value: "é"}}, want: []string{"c", "\xc3\xa9"}},
	} {
		t.Run(test.name, func(t *testing.T) {
			t.Parallel()
			q := client.Query(test.sql)
			q.DefaultDatasetID = "go_bigtable_row_key_filters"
			q.Parameters = test.parameters
			rows, err := q.Read(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			var got []string
			for _, row := range readRows(t, rows) {
				switch value := row[0].(type) {
				case []byte:
					got = append(got, string(value))
				case int64:
					got = append(got, fmt.Sprint(value))
				default:
					t.Fatalf("row = %v", row)
				}
			}
			slices.Sort(got)
			if !slices.Equal(got, test.want) {
				t.Fatalf("rows = %q, want %q", got, test.want)
			}
		})
	}
}

// A dry run validates a query against the stored schema without reading the source, as
// docs/external-tables.md describes, so it succeeds after the Bigtable table is deleted.
func TestBigtableDryRun(t *testing.T) {
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
	source := parent + "/tables/go-bigtable-dry-run"
	if _, err := admin.CreateTable(ctx, &adminpb.CreateTableRequest{
		Parent: parent, TableId: "go-bigtable-dry-run",
		Table: &adminpb.Table{ColumnFamilies: map[string]*adminpb.ColumnFamily{"family": {}}},
	}); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		_, _ = admin.DeleteTable(context.Background(), &adminpb.DeleteTableRequest{Name: source})
	})
	newDataset(t, client, "go_bigtable_dry_run")
	if err := wait(ctx, client.Query(fmt.Sprintf(`CREATE EXTERNAL TABLE go_bigtable_dry_run.t
  OPTIONS (format = 'CLOUD_BIGTABLE', uris = 'https://googleapis.com/bigtable/%s')`, source))); err != nil {
		t.Fatal(err)
	}
	if _, err := admin.DeleteTable(ctx, &adminpb.DeleteTableRequest{Name: source}); err != nil {
		t.Fatal(err)
	}
	const sql = "SELECT rowkey, family.column FROM go_bigtable_dry_run.t WHERE rowkey = b'a'"
	q := client.Query(sql)
	q.DryRun = true
	job, err := q.Run(ctx)
	if err == nil {
		err = job.LastStatus().Err()
	}
	if err != nil {
		t.Fatalf("dry run: %v", err)
	}
	statistics, ok := job.LastStatus().Statistics.Details.(*bigquery.QueryStatistics)
	if !ok {
		t.Fatalf("dry run statistics = %T", job.LastStatus().Statistics.Details)
	}
	if len(statistics.Schema) != 2 || statistics.Schema[0].Name != "rowkey" || statistics.Schema[0].Type != bigquery.BytesFieldType ||
		statistics.Schema[1].Name != "column" || !statistics.Schema[1].Repeated {
		t.Errorf("dry run schema = %v, want rowkey BYTES and REPEATED column", statistics.Schema)
	}
	// The query itself reads the deleted source.
	if _, err := client.Query(sql).Read(ctx); err == nil {
		t.Fatal("query of a deleted Bigtable table succeeded")
	}
}
