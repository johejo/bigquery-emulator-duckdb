package goclient

import (
	"context"
	"math/big"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

// wait runs a job and returns its error.
func wait(ctx context.Context, runner interface {
	Run(context.Context) (*bigquery.Job, error)
}) error {
	job, err := runner.Run(ctx)
	if err != nil {
		return err
	}
	status, err := job.Wait(ctx)
	if err != nil {
		return err
	}
	return status.Err()
}

// Columns keep the rounding mode that DDL and tables.insert give them, and DML rounds the values
// it writes to a parameterized column with it. The expected values are the examples of BigQuery's
// DDL reference (2.25 and -2.25 to one digit) and of its RoundingMode enum (1.5 and 2.5 to none).
func TestColumnRoundingModes(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_rounding_mode")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	run := func(sql string) error {
		_, _, err := runScript(t, client, dataset.DatasetID, sql)
		return err
	}
	read := func(sql string) [][]bigquery.Value {
		t.Helper()
		query := client.Query(sql)
		query.DefaultDatasetID = dataset.DatasetID
		rows, err := query.Read(ctx)
		if err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
		var result [][]bigquery.Value
		for {
			var row []bigquery.Value
			if err := rows.Next(&row); err != nil {
				break
			}
			result = append(result, row)
		}
		return result
	}

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
		"INSERT t (s) VALUES (STRUCT(NUMERIC '1'))": "",
		"CREATE TABLE r (s STRUCT<n NUMERIC(5, 1) OPTIONS (rounding_mode = 'ROUND_HALF_EVEN')>); INSERT r (s) VALUES (STRUCT(2.25))": "writing a STRUCT or ARRAY with parameterized ROUND_HALF_EVEN values",
		"CREATE TABLE r (n NUMERIC(5, 1) DEFAULT 1 OPTIONS (rounding_mode = 'ROUND_HALF_EVEN'))":                                     "defaults on parameterized ROUND_HALF_EVEN fields",
		"CREATE TABLE r (n INT64 OPTIONS (rounding_mode = 'ROUND_HALF_EVEN'))":                                                       "rounding modes on INTEGER fields",
		"INSERT api (even) SELECT NUMERIC '2.25'":                                                                                    "",
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
	insert := dataset.Table("api").Inserter().Put(ctx, &bigquery.ValuesSaver{Schema: api, Row: []bigquery.Value{big.NewRat(9, 4), nil}})
	if insert == nil || !strings.Contains(insert.Error(), "tabledata.insertAll into ROUND_HALF_EVEN columns") {
		t.Errorf("insertAll: error = %v", insert)
	}
	source := bigquery.NewReaderSource(strings.NewReader("{\"even\": 2.25}\n"))
	source.SourceFormat = bigquery.JSON
	if err := wait(ctx, dataset.Table("api").LoaderFrom(source)); err == nil ||
		!strings.Contains(err.Error(), "load jobs into ROUND_HALF_EVEN columns") {
		t.Errorf("load: error = %v", err)
	}
	query := client.Query("SELECT NUMERIC '2.25' AS even, BIGNUMERIC '1' AS away")
	query.Dst = dataset.Table("api")
	query.WriteDisposition = bigquery.WriteAppend
	if err := wait(ctx, query); err == nil || !strings.Contains(err.Error(), "appending rows of another schema to ROUND_HALF_EVEN columns") {
		t.Errorf("query append: error = %v", err)
	}
	// A copy writes values that are already rounded.
	if err := wait(ctx, dataset.Table("copied").CopierFrom(dataset.Table("api"))); err != nil {
		t.Errorf("copy: %v", err)
	}
}
