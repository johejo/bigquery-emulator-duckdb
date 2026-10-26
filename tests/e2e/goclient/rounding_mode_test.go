package goclient

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// jsonRow is a tabledata.insertAll row the client sends as it is.
type jsonRow map[string]bigquery.Value

func (r jsonRow) Save() (map[string]bigquery.Value, string, error) {
	return r, bigquery.NoDedupeID, nil
}

// Columns keep the rounding mode that DDL and tables.insert give them, and DML rounds the values
// it writes to a parameterized column with it. The expected values are the examples of BigQuery's
// DDL reference (2.25 and -2.25 to one digit) and of its RoundingMode enum (1.5 and 2.5 to none).
func TestColumnRoundingModes(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := newDataset(t, client, "go_rounding_mode")
	p := datasetQueries{t: t, client: client, dataset: dataset}
	run, read := p.run, p.read

	for _, sql := range []string{
		`CREATE TABLE t (id INT64,
		 even NUMERIC(5, 1) OPTIONS (rounding_mode = 'ROUND_HALF_EVEN'),
		 away NUMERIC(5, 1) OPTIONS (rounding_mode = 'ROUND_HALF_AWAY_FROM_ZERO'),
		 unset NUMERIC(5, 1),
		 big BIGNUMERIC(10, 0) OPTIONS (rounding_mode = 'ROUND_HALF_EVEN'),
		 s STRUCT<n NUMERIC OPTIONS (rounding_mode = 'ROUND_HALF_EVEN')>)`,
		"INSERT t (id, even, away, unset, big) VALUES (1, 2.25, 2.25, 2.25, 1.5), (2, -2.25, -2.25, -2.25, 2.5)",
		"INSERT t (id, even, away, unset, big) SELECT 3, even, away, unset, big FROM t WHERE id = 1",
		"UPDATE t SET even = -even, big = 2.5 WHERE id = 3",
		`MERGE t USING (SELECT 4 AS id, NUMERIC '2.25' AS v) AS s ON t.id = s.id
		 WHEN NOT MATCHED THEN INSERT (id, even, big) VALUES (s.id, s.v, 1.5)`,
		"CREATE TABLE ctas (even NUMERIC(5, 1) OPTIONS (rounding_mode = 'ROUND_HALF_EVEN')) AS SELECT NUMERIC '2.25' AS even",
	} {
		if err := run(sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	got := read("SELECT id, CAST(even AS STRING), CAST(away AS STRING), CAST(unset AS STRING), CAST(big AS STRING) FROM t ORDER BY id")
	want := [][]bigquery.Value{
		{int64(1), "2.2", "2.3", "2.3", "2"},
		{int64(2), "-2.2", "-2.3", "-2.3", "2"},
		{int64(3), "-2.2", "2.3", "2.3", "2"},
		{int64(4), "2.2", nil, nil, "2"},
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("rounded rows = %v, want %v", got, want)
	}
	if got := read("SELECT CAST(even AS STRING) FROM ctas"); !reflect.DeepEqual(got, [][]bigquery.Value{{"2.2"}}) {
		t.Errorf("CREATE TABLE AS SELECT = %v", got)
	}

	// The rounding modes read back from tables.get and INFORMATION_SCHEMA, and ALTER COLUMN SET
	// OPTIONS changes them.
	metadata, err := dataset.Table("t").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	modes := []bigquery.RoundingMode{}
	for _, field := range metadata.Schema {
		modes = append(modes, field.RoundingMode)
	}
	modes = append(modes, metadata.Schema[5].Schema[0].RoundingMode)
	if want := []bigquery.RoundingMode{"", bigquery.RoundHalfEven, bigquery.RoundHalfAwayFromZero, "",
		bigquery.RoundHalfEven, "", bigquery.RoundHalfEven}; !reflect.DeepEqual(modes, want) {
		t.Errorf("tables.get rounding modes = %v, want %v", modes, want)
	}
	if err := run("ALTER TABLE t ALTER COLUMN even SET OPTIONS (rounding_mode = NULL), ALTER COLUMN unset SET OPTIONS (rounding_mode = 'ROUND_HALF_EVEN')"); err != nil {
		t.Fatal(err)
	}
	got = read(`SELECT field_path, rounding_mode FROM INFORMATION_SCHEMA.COLUMN_FIELD_PATHS
		WHERE table_name = 't' AND rounding_mode IS NOT NULL ORDER BY field_path`)
	want = [][]bigquery.Value{{"away", "ROUND_HALF_AWAY_FROM_ZERO"}, {"big", "ROUND_HALF_EVEN"},
		{"s.n", "ROUND_HALF_EVEN"}, {"unset", "ROUND_HALF_EVEN"}}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("COLUMN_FIELD_PATHS = %v, want %v", got, want)
	}
	if err := run("INSERT t (id, even, unset) VALUES (5, 2.25, 2.25)"); err != nil {
		t.Fatal(err)
	}
	got = read("SELECT CAST(even AS STRING), CAST(unset AS STRING) FROM t WHERE id = 5")
	if !reflect.DeepEqual(got, [][]bigquery.Value{{"2.3", "2.2"}}) {
		t.Errorf("after ALTER COLUMN SET OPTIONS = %v", got)
	}

	api := bigquery.Schema{
		{Name: "even", Type: bigquery.NumericFieldType, Precision: 5, Scale: 1, RoundingMode: bigquery.RoundHalfEven},
		{Name: "away", Type: bigquery.BigNumericFieldType, RoundingMode: bigquery.RoundHalfAwayFromZero},
	}
	if err := dataset.Table("api").Create(ctx, &bigquery.TableMetadata{Schema: api}); err != nil {
		t.Fatal(err)
	}
	metadata, err = dataset.Table("api").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if metadata.Schema[0].RoundingMode != bigquery.RoundHalfEven ||
		metadata.Schema[1].RoundingMode != bigquery.RoundHalfAwayFromZero {
		t.Errorf("tables.insert schema = %+v", metadata.Schema)
	}
	got = read(`SELECT column_name, rounding_mode FROM INFORMATION_SCHEMA.COLUMNS
		WHERE table_name = 'api' ORDER BY ordinal_position`)
	want = [][]bigquery.Value{{"even", "ROUND_HALF_EVEN"}, {"away", "ROUND_HALF_AWAY_FROM_ZERO"}}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("COLUMNS = %v, want %v", got, want)
	}

	// Writes the emulator does not round to even are rejected.
	for sql, want := range map[string]string{
		"INSERT t (s) VALUES (STRUCT(NUMERIC '1'))":                                              "",
		"CREATE TABLE r (n NUMERIC(5, 1) DEFAULT 1 OPTIONS (rounding_mode = 'ROUND_HALF_EVEN'))": "",
		"CREATE TABLE r (n INT64 OPTIONS (rounding_mode = 'ROUND_HALF_EVEN'))":                   "rounding modes on INTEGER fields",
		"INSERT api (even) SELECT NUMERIC '2.25'":                                                "",
	} {
		err := run(sql)
		if want == "" {
			if err != nil {
				t.Errorf("%s: %v", sql, err)
			}
		} else if err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: error = %v, want one containing %q", sql, err, want)
		}
	}
	// A copy writes values that are already rounded.
	if err := wait(ctx, dataset.Table("copied").CopierFrom(dataset.Table("api"))); err != nil {
		t.Errorf("copy: %v", err)
	}
}

// Query and load jobs that append to a table and tabledata.insertAll round the values of its fields
// with their rounding modes, as DML does, and fail on a value out of a field's range, as BigQuery
// answers.
func TestAppendedRoundingModes(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := newDataset(t, client, "go_appended_rounding_mode")
	numeric := func(name string, typ bigquery.FieldType, precision, scale int64, mode bigquery.RoundingMode) *bigquery.FieldSchema {
		return &bigquery.FieldSchema{Name: name, Type: typ, Precision: precision, Scale: scale, RoundingMode: mode}
	}
	flat := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		numeric("even", bigquery.NumericFieldType, 5, 1, bigquery.RoundHalfEven),
		numeric("away", bigquery.NumericFieldType, 5, 1, ""),
		numeric("big", bigquery.BigNumericFieldType, 10, 0, bigquery.RoundHalfEven),
		numeric("big_away", bigquery.BigNumericFieldType, 10, 0, ""),
	}
	array := numeric("a", bigquery.NumericFieldType, 5, 1, bigquery.RoundHalfEven)
	array.Repeated = true
	nested := append(append(bigquery.Schema{}, flat...),
		&bigquery.FieldSchema{Name: "s", Type: bigquery.RecordFieldType, Schema: bigquery.Schema{
			numeric("n", bigquery.NumericFieldType, 5, 1, bigquery.RoundHalfEven),
		}},
		array)
	for name, schema := range map[string]bigquery.Schema{"flat": flat, "nested": nested} {
		if err := dataset.Table(name).Create(ctx, &bigquery.TableMetadata{Schema: schema}); err != nil {
			t.Fatal(err)
		}
	}
	appendQuery := func(table, sql string) error {
		query := client.Query(sql)
		query.Dst = dataset.Table(table)
		query.WriteDisposition = bigquery.WriteAppend
		return wait(ctx, query)
	}
	load := func(table string, format bigquery.DataFormat, data string) error {
		source := bigquery.NewReaderSource(strings.NewReader(data))
		source.SourceFormat = format
		return wait(ctx, dataset.Table(table).LoaderFrom(source))
	}

	if err := appendQuery("nested", `SELECT 1 AS id, NUMERIC '2.25' AS even, NUMERIC '2.25' AS away,
		BIGNUMERIC '2.5' AS big, STRUCT(NUMERIC '2.25' AS n) AS s,
		[NUMERIC '2.25', NUMERIC '-2.25', NUMERIC '2.35'] AS a`); err != nil {
		t.Fatalf("query append: %v", err)
	}
	if err := load("nested", bigquery.JSON, `{"id": 2, "even": 2.25, "away": 2.25, "big": 2.5, "big_away": 2.5, "s": {"n": 2.25}, "a": [2.25, -2.25, 2.35]}
{"id": 3, "even": "-2.25", "away": "2.25"}
`); err != nil {
		t.Fatalf("JSON load: %v", err)
	}
	if err := load("flat", bigquery.CSV, "4,2.25,2.25,2.5,2.5\n5,-2.25,,,\n"); err != nil {
		t.Fatalf("CSV load: %v", err)
	}
	// Rows go to tabledata.insertAll as given: JSON numbers or strings.
	insert := func(rows ...jsonRow) error {
		var savers []bigquery.ValueSaver
		for _, row := range rows {
			savers = append(savers, row)
		}
		return dataset.Table("nested").Inserter().Put(ctx, savers)
	}
	if err := insert(
		jsonRow{"id": 6, "even": 2.25, "away": 2.25, "big": 2.5, "big_away": 2.5,
			"s": map[string]any{"n": 2.25}, "a": []any{2.25, -2.25, 2.35}},
		jsonRow{"id": 7, "even": "-2.25", "away": "2.25", "a": []any{"2.35"}},
	); err != nil {
		t.Fatalf("insertAll: %v", err)
	}
	query := client.Query(`SELECT id, CAST(even AS STRING), CAST(away AS STRING), CAST(big AS STRING),
		CAST(big_away AS STRING), CAST(s.n AS STRING),
		ARRAY_TO_STRING(ARRAY(SELECT CAST(x AS STRING) FROM UNNEST(a) AS x WITH OFFSET ORDER BY offset), ',')
		FROM go_appended_rounding_mode.nested
		UNION ALL
		SELECT id, CAST(even AS STRING), CAST(away AS STRING), CAST(big AS STRING),
		CAST(big_away AS STRING), NULL, NULL FROM go_appended_rounding_mode.flat
		ORDER BY id`)
	rows, err := query.Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	got := readRows(t, rows)
	want := [][]bigquery.Value{
		{int64(1), "2.2", "2.3", "2", nil, "2.2", "2.2,-2.2,2.4"},
		{int64(2), "2.2", "2.3", "2", "3", "2.2", "2.2,-2.2,2.4"},
		{int64(3), "-2.2", "2.3", nil, nil, nil, ""},
		{int64(4), "2.2", "2.3", "2", "3", nil, nil},
		{int64(5), "-2.2", nil, nil, nil, nil, nil},
		{int64(6), "2.2", "2.3", "2", "3", "2.2", "2.2,-2.2,2.4"},
		{int64(7), "-2.2", "2.3", nil, nil, nil, "2.4"},
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("appended rows = %v, want %v", got, want)
	}

	const outOfRange = "NUMERIC(5, 1) has precision 5 and scale 1 but got a value that is not in range of [-9999.9, 9999.9]"
	for name, err := range map[string]error{
		"query append": appendQuery("flat", "SELECT NUMERIC '9999.95' AS even"),
		"CSV load":     load("flat", bigquery.CSV, "6,9999.95,,,\n"),
		"JSON load":    load("flat", bigquery.JSON, `{"id": 6, "even": 9999.95, "away": 1, "big": 1, "big_away": 1}`+"\n"),
	} {
		if err == nil || !strings.Contains(err.Error(), outOfRange) {
			t.Errorf("%s of 9999.95: error = %v, want one containing %q", name, err, outOfRange)
		}
	}
	var rowErrors bigquery.PutMultiError
	if err := insert(jsonRow{"id": 8, "even": 9999.95}); !errors.As(err, &rowErrors) ||
		len(rowErrors) != 1 || !strings.Contains(rowErrors[0].Error(), outOfRange) {
		t.Errorf("insertAll of 9999.95: error = %v, want a row error containing %q", err, outOfRange)
	}
}

// A table created in a dataset "will inherit the default rounding mode of the dataset", unless it
// sets its own, and "Existing tables in the dataset are unaffected" by a change; ALTER SCHEMA SET
// OPTIONS "has no impact on new columns added to existing tables". A NUMERIC or BIGNUMERIC field
// added to a table "without an explicit rounding mode specified" inherits the table's default, and
// "Changing this field doesn't affect existing fields"
// (https://cloud.google.com/bigquery/docs/reference/rest/v2/datasets,
// https://cloud.google.com/bigquery/docs/reference/rest/v2/tables,
// https://cloud.google.com/bigquery/docs/updating-datasets). The rounded values are the examples
// of BigQuery's DDL reference, 2.25 to 2.2 and 2.3.
func TestDefaultRoundingModes(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_default_rounding_mode")
	_ = dataset.DeleteWithContents(ctx)
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := datasetQueries{t: t, client: client, dataset: dataset}
	run, mustRun, read := p.run, p.mustRun, p.read
	// The rounding modes tables.get reports for the fields of `table`, by path.
	modes := func(table string) map[string]bigquery.RoundingMode {
		t.Helper()
		metadata, err := dataset.Table(table).Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		got := map[string]bigquery.RoundingMode{}
		var walk func(prefix string, schema bigquery.Schema)
		walk = func(prefix string, schema bigquery.Schema) {
			for _, field := range schema {
				got[prefix+field.Name] = field.RoundingMode
				walk(prefix+field.Name+".", field.Schema)
			}
		}
		walk("", metadata.Schema)
		return got
	}
	const even, away = bigquery.RoundHalfEven, bigquery.RoundHalfAwayFromZero

	if _, _, err := runScript(t, client, "", "CREATE SCHEMA go_default_rounding_mode OPTIONS (default_rounding_mode = 'ROUND_HALF_EVEN')"); err != nil {
		t.Fatal(err)
	}
	for _, sql := range []string{
		`CREATE TABLE t (n NUMERIC(5, 1),
		 away NUMERIC(5, 1) OPTIONS (rounding_mode = 'ROUND_HALF_AWAY_FROM_ZERO'),
		 s STRUCT<b BIGNUMERIC>, i INT64)`,
		"CREATE TABLE own (n NUMERIC(5, 1)) OPTIONS (default_rounding_mode = 'ROUND_HALF_AWAY_FROM_ZERO')",
		"CREATE TABLE q AS SELECT NUMERIC '2.25' AS n",
		`CREATE TABLE ctas (n NUMERIC(5, 1)) OPTIONS (default_rounding_mode = 'ROUND_HALF_EVEN')
		 AS SELECT NUMERIC '2.25' AS n`,
	} {
		mustRun(sql)
	}
	api := bigquery.Schema{{Name: "n", Type: bigquery.NumericFieldType}}
	if err := dataset.Table("api").Create(ctx, &bigquery.TableMetadata{Schema: api}); err != nil {
		t.Fatal(err)
	}

	// Forms whose outcome BigQuery does not document, and writes the emulator does not round to
	// even, are rejected.
	for sql, want := range map[string]string{
		"CREATE VIEW v OPTIONS (default_rounding_mode = 'ROUND_HALF_EVEN') AS SELECT 1 AS x": "CREATE VIEW option default_rounding_mode",
	} {
		if err := run(sql); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: error = %v, want one containing %q", sql, err, want)
		}
	}
	source := bigquery.NewReaderSource(strings.NewReader("{\"n\": 2.25}\n"))
	source.SourceFormat = bigquery.JSON
	source.Schema = api
	if err := wait(ctx, dataset.Table("loaded").LoaderFrom(source)); err != nil {
		t.Fatalf("load into unparameterized fields: %v", err)
	}

	// Later defaults apply to tables and fields added afterwards only.
	for _, sql := range []string{
		"ALTER SCHEMA go_default_rounding_mode SET OPTIONS (default_rounding_mode = 'ROUND_HALF_AWAY_FROM_ZERO')",
		"ALTER TABLE t ADD COLUMN added NUMERIC(5, 1)",
		"CREATE TABLE later (n NUMERIC(5, 1))",
		"ALTER TABLE own SET OPTIONS (default_rounding_mode = 'ROUND_HALF_EVEN')",
		"ALTER TABLE own ADD COLUMN m NUMERIC(5, 1)",
	} {
		mustRun(sql)
	}
	metadata, err := dataset.Table("t").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	update := bigquery.TableMetadataToUpdate{
		Schema: append(metadata.Schema, &bigquery.FieldSchema{Name: "patched", Type: bigquery.BigNumericFieldType}),
	}
	if _, err := dataset.Table("t").Update(ctx, update, metadata.ETag); err != nil {
		t.Fatal(err)
	}

	for table, want := range map[string]map[string]bigquery.RoundingMode{
		"t":      {"n": even, "away": away, "s": "", "s.b": even, "i": "", "added": even, "patched": even},
		"own":    {"n": away, "m": even},
		"q":      {"n": even},
		"ctas":   {"n": even},
		"api":    {"n": even},
		"loaded": {"n": even},
		"later":  {"n": away},
	} {
		if got := modes(table); !reflect.DeepEqual(got, want) {
			t.Errorf("%s rounding modes = %v, want %v", table, got, want)
		}
	}
	mustRun("INSERT t (n, away, added) VALUES (2.25, 2.25, 2.25); INSERT own (n, m) VALUES (2.25, 2.25)")
	got := read(`SELECT 1 AS k, CAST(n AS STRING), CAST(away AS STRING), CAST(added AS STRING) FROM t
		UNION ALL SELECT 2, CAST(n AS STRING), CAST(m AS STRING), NULL FROM own
		UNION ALL SELECT 3, CAST(n AS STRING), NULL, NULL FROM ctas
		UNION ALL SELECT 4, CAST(n AS STRING), NULL, NULL FROM loaded ORDER BY k`)
	want := [][]bigquery.Value{{int64(1), "2.2", "2.3", "2.2"}, {int64(2), "2.3", "2.2", nil}, {int64(3), "2.2", nil, nil}, {int64(4), "2.25", nil, nil}}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("rounded rows = %v, want %v", got, want)
	}
}

// CREATE TABLE LIKE, COPY and CLONE keep the source's default rounding mode, which a column added
// to the copy later takes, unless their OPTIONS set another, and the rounding modes of the source's
// columns. In a dataset with a default, LIKE takes it when the source has none, and COPY and CLONE
// do not; BigQuery's answers to cases that were unverified.
func TestCopiedDefaultRoundingModes(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	plain := client.Dataset("go_default_rounding_plain")
	even := client.Dataset("go_default_rounding_even")
	for _, dataset := range []*bigquery.Dataset{plain, even} {
		_ = dataset.DeleteWithContents(ctx)
		t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	}
	run := func(sql string) error {
		_, _, err := runScript(t, client, "", sql)
		return err
	}
	read := func(sql string) []bigquery.Value {
		t.Helper()
		rows, err := client.Query(sql).Read(ctx)
		if err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
		var row []bigquery.Value
		if err := rows.Next(&row); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
		return row
	}
	for _, sql := range []string{
		"CREATE SCHEMA go_default_rounding_plain",
		"CREATE SCHEMA go_default_rounding_even OPTIONS (default_rounding_mode = 'ROUND_HALF_EVEN')",
		"CREATE TABLE go_default_rounding_plain.src (n NUMERIC(5, 1))",
		"CREATE TABLE go_default_rounding_plain.default_src (i INT64) OPTIONS (default_rounding_mode = 'ROUND_HALF_EVEN')",
	} {
		if err := run(sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	for kind, added := range map[string]string{"LIKE": "2.2", "COPY": "2.3", "CLONE": "2.3"} {
		copied := "go_default_rounding_even." + strings.ToLower(kind) + "_dst"
		if err := run("CREATE TABLE " + copied + " " + kind + " go_default_rounding_plain.src; " +
			"ALTER TABLE " + copied + " ADD COLUMN m NUMERIC(5, 1); " +
			"INSERT " + copied + " (n, m) VALUES (2.25, 2.25)"); err != nil {
			t.Fatalf("%s: %v", kind, err)
		}
		got := read("SELECT CAST(n AS STRING), CAST(m AS STRING) FROM " + copied)
		if want := []bigquery.Value{"2.3", added}; !reflect.DeepEqual(got, want) {
			t.Errorf("%s in a dataset with a default: %v, want %v", kind, got, want)
		}

		copied = "go_default_rounding_plain." + strings.ToLower(kind) + "_default"
		if err := run("CREATE TABLE " + copied + " " + kind + " go_default_rounding_plain.default_src; " +
			"ALTER TABLE " + copied + " ADD COLUMN n NUMERIC(5, 1); " +
			"INSERT " + copied + " (n) VALUES (2.25)"); err != nil {
			t.Fatalf("%s: %v", kind, err)
		}
		if got := read("SELECT CAST(n AS STRING) FROM " + copied); !reflect.DeepEqual(got, []bigquery.Value{"2.2"}) {
			t.Errorf("%s of a table with a default: %v, want [2.2]", kind, got)
		}
	}
	// A default that OPTIONS set replaces the source's, and only columns added later take it.
	for _, kind := range []string{"LIKE", "COPY", "CLONE"} {
		copied := "go_default_rounding_plain." + strings.ToLower(kind) + "_options"
		if err := run("CREATE TABLE " + copied + " " + kind + " go_default_rounding_plain.src" +
			" OPTIONS (default_rounding_mode = 'ROUND_HALF_EVEN'); " +
			"ALTER TABLE " + copied + " ADD COLUMN m NUMERIC(5, 1); " +
			"INSERT " + copied + " (n, m) VALUES (2.25, 2.25)"); err != nil {
			t.Fatalf("%s with OPTIONS: %v", kind, err)
		}
		got := read("SELECT CAST(n AS STRING), CAST(m AS STRING) FROM " + copied)
		if want := []bigquery.Value{"2.3", "2.2"}; !reflect.DeepEqual(got, want) {
			t.Errorf("%s with OPTIONS: %v, want %v", kind, got, want)
		}
	}
	if err := run("CREATE TABLE go_default_rounding_plain.like_replaced LIKE go_default_rounding_plain.default_src" +
		" OPTIONS (default_rounding_mode = 'ROUND_HALF_AWAY_FROM_ZERO'); " +
		"ALTER TABLE go_default_rounding_plain.like_replaced ADD COLUMN n NUMERIC(5, 1); " +
		"INSERT go_default_rounding_plain.like_replaced (n) VALUES (2.25)"); err != nil {
		t.Fatal(err)
	}
	if got := read("SELECT CAST(n AS STRING) FROM go_default_rounding_plain.like_replaced"); !reflect.DeepEqual(got, []bigquery.Value{"2.3"}) {
		t.Errorf("LIKE replacing the source's default: %v, want [2.3]", got)
	}
}

// tables.get as the REST API returns it, since the Go client leaves out defaultRoundingMode.
type roundingResource struct {
	DefaultRoundingMode string `json:"defaultRoundingMode"`
	Schema              struct {
		Fields []struct {
			Name         string `json:"name"`
			RoundingMode string `json:"roundingMode"`
		} `json:"fields"`
	} `json:"schema"`
}

// The default rounding mode of `table` and the rounding modes of its fields, by name.
func tableRoundingModes(t *testing.T, client *bigquery.Client, dataset, table string) (string, map[string]string) {
	t.Helper()
	url := fmt.Sprintf("%s/bigquery/v2/projects/%s/datasets/%s/tables/%s",
		os.Getenv("BQ_EMULATOR_API"), client.Project(), dataset, table)
	response, err := http.Get(url)
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	var resource roundingResource
	if err := json.NewDecoder(response.Body).Decode(&resource); err != nil {
		t.Fatal(err)
	}
	fields := map[string]string{}
	for _, field := range resource.Schema.Fields {
		fields[field.Name] = field.RoundingMode
	}
	return resource.DefaultRoundingMode, fields
}

// Copy jobs keep the rounding modes of the source's fields, but their new table takes no default
// rounding mode, neither the source's nor the dataset's. Replacing a table keeps its default,
// and copy and query results and an unparameterized load schema do not inherit it. Appending
// keeps it too. BigQuery's answers, which bq show reported for the same jobs.
func TestJobDefaultRoundingModes(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	plain := client.Dataset("go_job_rounding_plain")
	even := client.Dataset("go_job_rounding_even")
	for _, dataset := range []*bigquery.Dataset{plain, even} {
		_ = dataset.DeleteWithContents(ctx)
		t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	}
	if _, _, err := runScript(t, client, "", `
		CREATE SCHEMA go_job_rounding_plain;
		CREATE SCHEMA go_job_rounding_even OPTIONS (default_rounding_mode = 'ROUND_HALF_EVEN');
		CREATE TABLE go_job_rounding_plain.src (n NUMERIC(5, 1));
		CREATE TABLE go_job_rounding_plain.default_src (n NUMERIC(5, 1))
		  OPTIONS (default_rounding_mode = 'ROUND_HALF_AWAY_FROM_ZERO');
		CREATE TABLE go_job_rounding_plain.copy_truncated (n NUMERIC(5, 1))
		  OPTIONS (default_rounding_mode = 'ROUND_HALF_EVEN');
		CREATE TABLE go_job_rounding_plain.query_truncated (n NUMERIC(5, 1))
		  OPTIONS (default_rounding_mode = 'ROUND_HALF_EVEN');
		CREATE TABLE go_job_rounding_plain.load_truncated (n NUMERIC(5, 1))
		  OPTIONS (default_rounding_mode = 'ROUND_HALF_EVEN');
		CREATE TABLE go_job_rounding_plain.appended (n NUMERIC(5, 1)
		  OPTIONS (rounding_mode = 'ROUND_HALF_AWAY_FROM_ZERO'))`); err != nil {
		t.Fatal(err)
	}
	copyJob := func(source, destination *bigquery.Table, write bigquery.TableWriteDisposition) {
		t.Helper()
		copier := destination.CopierFrom(source)
		copier.WriteDisposition = write
		if err := wait(ctx, copier); err != nil {
			t.Fatalf("copy to %s: %v", destination.TableID, err)
		}
	}
	copyJob(plain.Table("default_src"), plain.Table("default_copy"), bigquery.WriteEmpty)
	copyJob(plain.Table("src"), even.Table("copy"), bigquery.WriteEmpty)
	copyJob(plain.Table("default_src"), even.Table("default_copy"), bigquery.WriteEmpty)
	copyJob(plain.Table("src"), plain.Table("copy_truncated"), bigquery.WriteTruncate)
	copyJob(plain.Table("default_src"), plain.Table("appended"), bigquery.WriteAppend)
	query := client.Query("SELECT NUMERIC '1' AS n")
	query.Dst = plain.Table("query_truncated")
	query.WriteDisposition = bigquery.WriteTruncate
	if err := wait(ctx, query); err != nil {
		t.Fatal(err)
	}
	source := bigquery.NewReaderSource(strings.NewReader("{\"n\": 1}\n"))
	source.SourceFormat = bigquery.JSON
	source.Schema = bigquery.Schema{{Name: "n", Type: bigquery.NumericFieldType}}
	loader := plain.Table("load_truncated").LoaderFrom(source)
	loader.WriteDisposition = bigquery.WriteTruncate
	if err := wait(ctx, loader); err != nil {
		t.Fatal(err)
	}

	const evenMode, away = "ROUND_HALF_EVEN", "ROUND_HALF_AWAY_FROM_ZERO"
	for _, c := range []struct {
		dataset, table, defaultMode, n string
	}{
		{"go_job_rounding_plain", "default_copy", "", away},
		{"go_job_rounding_even", "copy", "", ""},
		{"go_job_rounding_even", "default_copy", "", away},
		{"go_job_rounding_plain", "copy_truncated", evenMode, ""},
		{"go_job_rounding_plain", "query_truncated", evenMode, ""},
		{"go_job_rounding_plain", "load_truncated", evenMode, ""},
		{"go_job_rounding_plain", "appended", "", away},
	} {
		defaultMode, fields := tableRoundingModes(t, client, c.dataset, c.table)
		if defaultMode != c.defaultMode || fields["n"] != c.n {
			t.Errorf("%s.%s: defaultRoundingMode = %q, n = %q; want %q, %q",
				c.dataset, c.table, defaultMode, fields["n"], c.defaultMode, c.n)
		}
	}
}

// BigQuery rejects a copy between different numeric schemas, rather than rounding the source's
// values to the destination's parameters or mode. A copy of the same schema needs no rounding.
func TestCopyRoundingModes(t *testing.T) {
	t.Parallel()
	ctx := t.Context()
	client := newClient(t)
	dataset := newDataset(t, client, "go_copy_rounding_modes")
	if _, _, err := runScript(t, client, dataset.DatasetID, `
	 CREATE TABLE src (n NUMERIC(5, 1) OPTIONS (rounding_mode = 'ROUND_HALF_EVEN'));
	 INSERT src VALUES (NUMERIC '2.25');
	 CREATE TABLE dst LIKE src;
	 CREATE TABLE different_precision (n NUMERIC(6, 1) OPTIONS (rounding_mode = 'ROUND_HALF_EVEN'));
	 CREATE TABLE different_mode (n NUMERIC(5, 1) OPTIONS (rounding_mode = 'ROUND_HALF_AWAY_FROM_ZERO'));
	 INSERT different_precision VALUES (1);
	 INSERT different_mode VALUES (1)`); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"src", "different_precision", "different_mode"} {
		copier := dataset.Table("dst").CopierFrom(dataset.Table(name))
		copier.WriteDisposition = bigquery.WriteAppend
		err := wait(ctx, copier)
		if name == "src" {
			if err != nil {
				t.Fatal(err)
			}
		} else if err == nil || strings.Contains(err.Error(), "The emulator does not support") {
			t.Errorf("copy from %s: got %v, want a schema error", name, err)
		}
	}
	rows, err := client.Query("SELECT COUNT(*), CAST(MAX(n) AS STRING) FROM " + dataset.DatasetID + ".dst").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var got []bigquery.Value
	if err := rows.Next(&got); err != nil {
		t.Fatal(err)
	}
	if want := []bigquery.Value{int64(1), "2.2"}; !reflect.DeepEqual(got, want) {
		t.Errorf("copy destination = %v, want %v", got, want)
	}
}

// Unparameterized decimal loads preserve their full scale regardless of the rounding mode.
// NUMERIC's scale is 9 and BIGNUMERIC's is 38 (BigQuery's data types reference).
func TestLoadedUnparameterizedRoundingModes(t *testing.T) {
	t.Parallel()
	client := newClient(t)
	ctx := t.Context()
	dataset := newDataset(t, client, "go_loaded_rounding_modes")
	schema := bigquery.Schema{
		{Name: "n", Type: bigquery.NumericFieldType, RoundingMode: bigquery.RoundHalfEven},
		{Name: "b", Type: bigquery.BigNumericFieldType, RoundingMode: bigquery.RoundHalfEven},
	}
	for _, format := range []bigquery.DataFormat{bigquery.CSV, bigquery.JSON} {
		t.Run(string(format), func(t *testing.T) {
			table := dataset.Table(strings.ToLower(string(format)))
			for _, disposition := range []bigquery.TableWriteDisposition{bigquery.WriteEmpty, bigquery.WriteTruncate} {
				data := "2.250000001,2.50000000000000000000000000000000000001\n"
				if format == bigquery.JSON {
					data = `{"n": "2.250000001", "b": "2.50000000000000000000000000000000000001"}`
				}
				source := bigquery.NewReaderSource(strings.NewReader(data))
				source.SourceFormat = format
				source.Schema = schema
				loader := table.LoaderFrom(source)
				loader.WriteDisposition = disposition
				if err := wait(ctx, loader); err != nil {
					t.Fatalf("%s load: %v", disposition, err)
				}
				metadata, err := table.Metadata(ctx)
				if err != nil {
					t.Fatal(err)
				}
				for _, field := range metadata.Schema {
					if field.RoundingMode != bigquery.RoundHalfEven || field.Precision != 0 {
						t.Errorf("loaded field = %+v", field)
					}
				}
				rows, err := client.Query("SELECT COUNT(*), CAST(MAX(n) AS STRING), CAST(MAX(b) AS STRING) FROM " + dataset.DatasetID + "." + table.TableID).Read(ctx)
				if err != nil {
					t.Fatal(err)
				}
				var got []bigquery.Value
				if err := rows.Next(&got); err != nil {
					t.Fatal(err)
				}
				if want := []bigquery.Value{int64(1), "2.250000001", "2.50000000000000000000000000000000000001"}; !reflect.DeepEqual(got, want) {
					t.Errorf("%s load = %v, want %v", disposition, got, want)
				}
			}
		})
	}
}

// Expected metadata and values were collected from BigQuery using the REST answer collector.
func TestUpdatedDefaultRoundingModes(t *testing.T) {
	t.Parallel()
	ctx := t.Context()
	client := newClient(t)
	dataset := newDataset(t, client, "go_updated_default_rounding")
	const even, away, unspecified = "ROUND_HALF_EVEN", "ROUND_HALF_AWAY_FROM_ZERO", "ROUNDING_MODE_UNSPECIFIED"
	for _, method := range []string{"PATCH", "PUT"} {
		for index, modes := range [][2]string{{away, even}, {even, away}, {even, unspecified}, {away, unspecified}} {
			t.Run(method+"/"+modes[0]+"/"+modes[1], func(t *testing.T) {
				table := fmt.Sprintf("t_%s_%d", strings.ToLower(method), index)
				sql := fmt.Sprintf("CREATE TABLE `%s.%s` (old NUMERIC(5,1)) OPTIONS(default_rounding_mode = '%s')", dataset.DatasetID, table, modes[0])
				if _, _, err := runScript(t, client, "", sql); err != nil {
					t.Fatal(err)
				}
				field := func(name, typ, precision, scale string) map[string]any {
					f := map[string]any{"name": name, "type": typ, "precision": precision}
					if scale != "" {
						f["scale"] = scale
					}
					return f
				}
				old := field("old", "NUMERIC", "5", "1")
				old["roundingMode"] = modes[0]
				schema := map[string]any{"fields": []any{old, field("added", "NUMERIC", "5", "1"), field("big", "BIGNUMERIC", "10", ""), map[string]any{"name": "record", "type": "RECORD", "fields": []any{field("nested", "NUMERIC", "5", "1")}}}}
				body, _ := json.Marshal(map[string]any{"tableReference": map[string]string{"projectId": client.Project(), "datasetId": dataset.DatasetID, "tableId": table}, "defaultRoundingMode": modes[1], "schema": schema})
				url := fmt.Sprintf("%s/bigquery/v2/projects/%s/datasets/%s/tables/%s", os.Getenv("BQ_EMULATOR_API"), client.Project(), dataset.DatasetID, table)
				request, err := http.NewRequestWithContext(ctx, method, url, strings.NewReader(string(body)))
				if err != nil {
					t.Fatal(err)
				}
				response, err := http.DefaultClient.Do(request)
				if err != nil {
					t.Fatal(err)
				}
				defer response.Body.Close()
				if response.StatusCode != 200 {
					var failure any
					_ = json.NewDecoder(response.Body).Decode(&failure)
					t.Fatalf("update: %d: %v", response.StatusCode, failure)
				}
				defaultMode, fields := tableRoundingModes(t, client, dataset.DatasetID, table)
				newMode, stored := modes[1], modes[1]
				if newMode == unspecified {
					newMode = ""
					stored = modes[0]
				}
				if defaultMode != stored || fields["old"] != modes[0] || fields["added"] != newMode || fields["big"] != newMode {
					t.Errorf("metadata = %s, %v", defaultMode, fields)
				}
				metadata, err := dataset.Table(table).Metadata(ctx)
				if err != nil {
					t.Fatal(err)
				}
				if string(metadata.Schema[3].Schema[0].RoundingMode) != newMode {
					t.Errorf("nested mode = %v", metadata.Schema[3].Schema[0].RoundingMode)
				}
				if _, _, err := runScript(t, client, dataset.DatasetID, "INSERT `"+table+"` VALUES (2.25,2.25,2.5,STRUCT(NUMERIC '2.25'))"); err != nil {
					t.Fatal(err)
				}
				rows, err := client.Query(fmt.Sprintf("SELECT CAST(old AS STRING),CAST(added AS STRING),CAST(big AS STRING),CAST(record.nested AS STRING) FROM `%s.%s`", dataset.DatasetID, table)).Read(ctx)
				if err != nil {
					t.Fatal(err)
				}
				var got []bigquery.Value
				if err := rows.Next(&got); err != nil {
					t.Fatal(err)
				}
				oldValue, value, bigValue := "2.3", "2.3", "3"
				if modes[0] == even {
					oldValue = "2.2"
				}
				if newMode == even {
					value = "2.2"
					bigValue = "2"
				}
				if want := []bigquery.Value{oldValue, value, bigValue, value}; !reflect.DeepEqual(got, want) {
					t.Errorf("values = %v, want %v", got, want)
				}
			})
		}
	}
}

func TestCreatedAndReplacedLoadRoundingModes(t *testing.T) {
	t.Parallel()
	ctx := t.Context()
	client := newClient(t)
	dataset := client.Dataset("go_created_load_rounding")
	_ = dataset.DeleteWithContents(ctx)
	if _, _, err := runScript(t, client, "", "CREATE SCHEMA "+dataset.DatasetID+" OPTIONS(default_rounding_mode='ROUND_HALF_EVEN')"); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(context.Background()) })
	for _, format := range []bigquery.DataFormat{bigquery.CSV, bigquery.JSON} {
		for _, explicit := range []bool{true, false} {
			for _, replacement := range []string{"create", "unchanged", "changed_precision", "different_column_mode", "removed_parameters"} {
				if explicit && replacement != "create" && replacement != "unchanged" {
					continue
				}
				replace := replacement != "create"
				name := fmt.Sprintf("%s_explicit_%t_%s", strings.ToLower(string(format)), explicit, replacement)
				t.Run(name, func(t *testing.T) {
					table := dataset.Table(name)
					schema := bigquery.Schema{{Name: "n", Type: bigquery.NumericFieldType, Precision: 5, Scale: 1}, {Name: "b", Type: bigquery.BigNumericFieldType, Precision: 10}, {Name: "away", Type: bigquery.NumericFieldType, Precision: 5, Scale: 1, RoundingMode: bigquery.RoundHalfAwayFromZero}}
					if replace {
						original := "NUMERIC(5,1)"
						if replacement == "changed_precision" {
							original = "NUMERIC(6,1)"
						}
						if replacement == "different_column_mode" {
							original += " OPTIONS(rounding_mode='ROUND_HALF_AWAY_FROM_ZERO')"
						}
						if _, _, err := runScript(t, client, dataset.DatasetID, "CREATE TABLE `"+name+"` (n "+original+",b BIGNUMERIC(10,0),away NUMERIC(5,1) OPTIONS(rounding_mode='ROUND_HALF_AWAY_FROM_ZERO')); INSERT `"+name+"` VALUES(7,7,7)"); err != nil {
							t.Fatal(err)
						}
					}
					if replacement == "removed_parameters" {
						for _, field := range schema {
							field.Precision = 0
							field.Scale = 0
						}
					}
					if explicit {
						schema[0].RoundingMode = bigquery.RoundHalfEven
						schema[1].RoundingMode = bigquery.RoundHalfEven
					}
					load := func(data string) error {
						source := bigquery.NewReaderSource(strings.NewReader(data))
						source.SourceFormat = format
						source.Schema = schema
						loader := table.LoaderFrom(source)
						if replace {
							loader.WriteDisposition = bigquery.WriteTruncate
						}
						return wait(ctx, loader)
					}
					data := "2.25,2.5,2.25\n-2.25,-2.5,-2.25\n2.35,1.5,2.35\n"
					if format == bigquery.JSON {
						data = "{\"n\":2.25,\"b\":2.5,\"away\":2.25}\n{\"n\":-2.25,\"b\":-2.5,\"away\":-2.25}\n{\"n\":2.35,\"b\":1.5,\"away\":2.35}\n"
					}
					if err := load(data); err != nil {
						t.Fatal(err)
					}
					metadata, err := table.Metadata(ctx)
					if err != nil {
						t.Fatal(err)
					}
					wantMode := bigquery.RoundHalfEven
					if replace && replacement != "unchanged" {
						wantMode = ""
					}
					if metadata.Schema[0].RoundingMode != wantMode || metadata.Schema[1].RoundingMode != wantMode || metadata.Schema[2].RoundingMode != bigquery.RoundHalfAwayFromZero {
						t.Errorf("loaded schema = %+v", metadata.Schema)
					}
					wantPrecision, wantScale := int64(5), int64(1)
					if replacement == "removed_parameters" {
						wantPrecision, wantScale = 0, 0
					}
					if metadata.Schema[0].Precision != wantPrecision || metadata.Schema[0].Scale != wantScale {
						t.Errorf("loaded NUMERIC parameters = (%d, %d), want (%d, %d)", metadata.Schema[0].Precision, metadata.Schema[0].Scale, wantPrecision, wantScale)
					}
					defaultMode, _ := tableRoundingModes(t, client, dataset.DatasetID, name)
					if defaultMode != "ROUND_HALF_EVEN" {
						t.Errorf("table default = %q", defaultMode)
					}
					rows, err := client.Query(fmt.Sprintf("SELECT CAST(n AS STRING),CAST(b AS STRING),CAST(away AS STRING) FROM `%s.%s` ORDER BY n", dataset.DatasetID, name)).Read(ctx)
					if err != nil {
						t.Fatal(err)
					}
					var got [][]bigquery.Value
					for {
						var row []bigquery.Value
						err := rows.Next(&row)
						if errors.Is(err, iterator.Done) {
							break
						}
						if err != nil {
							t.Fatal(err)
						}
						got = append(got, row)
					}
					want := [][]bigquery.Value{{"-2.3", "-3", "-2.3"}, {"2.3", "3", "2.3"}, {"2.4", "2", "2.4"}}
					if explicit {
						want = [][]bigquery.Value{{"-2.2", "-2", "-2.3"}, {"2.2", "2", "2.3"}, {"2.4", "2", "2.4"}}
					}
					if replacement == "removed_parameters" {
						want = [][]bigquery.Value{{"-2.25", "-2.5", "-2.25"}, {"2.25", "2.5", "2.25"}, {"2.35", "1.5", "2.35"}}
					}
					if !reflect.DeepEqual(got, want) {
						t.Errorf("loaded values = %v, want %v", got, want)
					}
				})
			}
		}
	}
}

// BigQuery's collected load answers cover nested numbers and strings, and show that a failed
// create leaves no table while a failed replacement preserves the original rows.
func TestLoadRoundingContainersAndOverflow(t *testing.T) {
	t.Parallel()
	ctx := t.Context()
	client := newClient(t)
	dataset := client.Dataset("go_load_rounding_boundaries")
	_ = dataset.DeleteWithContents(ctx)
	if _, _, err := runScript(t, client, "", "CREATE SCHEMA "+dataset.DatasetID+" OPTIONS(default_rounding_mode='ROUND_HALF_EVEN')"); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(context.Background()) })
	numeric := func(name string) *bigquery.FieldSchema {
		return &bigquery.FieldSchema{Name: name, Type: bigquery.NumericFieldType, Precision: 5, Scale: 1, RoundingMode: bigquery.RoundHalfEven}
	}
	for _, explicit := range []bool{true, false} {
		for _, stringsInput := range []bool{false, true} {
			name := fmt.Sprintf("nested_%t_%t", explicit, stringsInput)
			array := numeric("a")
			array.Repeated = true
			schema := bigquery.Schema{numeric("n"), {Name: "b", Type: bigquery.BigNumericFieldType, Precision: 10, RoundingMode: bigquery.RoundHalfEven}, {Name: "away", Type: bigquery.NumericFieldType, Precision: 5, Scale: 1, RoundingMode: bigquery.RoundHalfAwayFromZero}, {Name: "s", Type: bigquery.RecordFieldType, Schema: bigquery.Schema{numeric("n")}}, array}
			if !explicit {
				schema[0].RoundingMode = ""
				schema[1].RoundingMode = ""
				schema[3].Schema[0].RoundingMode = ""
				schema[4].RoundingMode = ""
			}
			data := `{"n":2.25,"b":2.5,"away":2.25,"s":{"n":-2.25},"a":[2.25,-2.25,2.35]}`
			if stringsInput {
				data = `{"n":"2.25","b":"2.5","away":"2.25","s":{"n":"-2.25"},"a":["2.25","-2.25","2.35"]}`
			}
			source := bigquery.NewReaderSource(strings.NewReader(data))
			source.SourceFormat = bigquery.JSON
			source.Schema = schema
			if err := wait(ctx, dataset.Table(name).LoaderFrom(source)); err != nil {
				t.Fatal(err)
			}
			rows, err := client.Query("SELECT CAST(n AS STRING),CAST(b AS STRING),CAST(away AS STRING),CAST(s.n AS STRING),ARRAY_TO_STRING(ARRAY(SELECT CAST(x AS STRING) FROM UNNEST(a) AS x WITH OFFSET ORDER BY offset),',') FROM " + dataset.DatasetID + "." + name).Read(ctx)
			if err != nil {
				t.Fatal(err)
			}
			var got []bigquery.Value
			if err := rows.Next(&got); err != nil {
				t.Fatal(err)
			}
			want := []bigquery.Value{"2.2", "2", "2.3", "-2.2", "2.2,-2.2,2.4"}
			if !explicit {
				want = []bigquery.Value{"2.3", "3", "2.3", "-2.3", "2.3,-2.3,2.4"}
			}
			if !reflect.DeepEqual(got, want) {
				t.Errorf("nested load = %v, want %v", got, want)
			}
		}
	}
	for _, format := range []bigquery.DataFormat{bigquery.CSV, bigquery.JSON} {
		for _, big := range []bool{false, true} {
			for _, replace := range []bool{false, true} {
				name := fmt.Sprintf("overflow_%s_%t_%t", strings.ToLower(string(format)), big, replace)
				schema := bigquery.Schema{numeric("n"), {Name: "b", Type: bigquery.BigNumericFieldType, Precision: 10, RoundingMode: bigquery.RoundHalfEven}, {Name: "away", Type: bigquery.NumericFieldType, Precision: 5, Scale: 1, RoundingMode: bigquery.RoundHalfAwayFromZero}}
				if replace {
					if _, _, err := runScript(t, client, dataset.DatasetID, "CREATE TABLE "+name+" (n NUMERIC(5,1),b BIGNUMERIC(10,0),away NUMERIC(5,1)); INSERT "+name+" VALUES(7,7,7)"); err != nil {
						t.Fatal(err)
					}
				}
				n, b := "9999.95", "1"
				if big {
					n, b = "1", "9999999999.5"
				}
				data := n + "," + b + ",1\n"
				if format == bigquery.JSON {
					data = `{"n":` + n + `,"b":` + b + `,"away":1}`
				}
				source := bigquery.NewReaderSource(strings.NewReader(data))
				source.SourceFormat = format
				source.Schema = schema
				table := dataset.Table(name)
				loader := table.LoaderFrom(source)
				if replace {
					loader.WriteDisposition = bigquery.WriteTruncate
				}
				if err := wait(ctx, loader); err == nil || !strings.Contains(err.Error(), "not in range") {
					t.Errorf("%s overflow = %v", name, err)
				}
				if !replace {
					if _, err := table.Metadata(ctx); err == nil {
						t.Errorf("failed load created %s", name)
					}
					continue
				}
				rows, err := client.Query("SELECT COUNT(*),CAST(MAX(n) AS STRING),CAST(MAX(b) AS STRING),CAST(MAX(away) AS STRING) FROM " + dataset.DatasetID + "." + name).Read(ctx)
				if err != nil {
					t.Fatal(err)
				}
				var got []bigquery.Value
				if err := rows.Next(&got); err != nil {
					t.Fatal(err)
				}
				if want := []bigquery.Value{int64(1), "7", "7", "7"}; !reflect.DeepEqual(got, want) {
					t.Errorf("failed replacement = %v, want %v", got, want)
				}
			}
		}
	}
}
