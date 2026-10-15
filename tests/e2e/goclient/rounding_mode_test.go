package goclient

import (
	"context"
	"encoding/json"
	"fmt"
	"math/big"
	"net/http"
	"os"
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
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_default_rounding_mode")
	_ = dataset.DeleteWithContents(ctx)
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	run := func(sql string) error {
		_, _, err := runScript(t, client, dataset.DatasetID, sql)
		return err
	}
	mustRun := func(sql string) {
		t.Helper()
		if err := run(sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
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
		"CREATE TABLE p (n NUMERIC(5, 1)) AS SELECT NUMERIC '2.25' AS n":                     "CREATE TABLE AS SELECT into parameterized columns that take ROUND_HALF_EVEN from the dataset",
		"CREATE VIEW v OPTIONS (default_rounding_mode = 'ROUND_HALF_EVEN') AS SELECT 1 AS x": "CREATE VIEW option default_rounding_mode",
	} {
		if err := run(sql); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: error = %v, want one containing %q", sql, err, want)
		}
	}
	source := bigquery.NewReaderSource(strings.NewReader("{\"n\": 2.25}\n"))
	source.SourceFormat = bigquery.JSON
	source.Schema = api
	if err := wait(ctx, dataset.Table("loaded").LoaderFrom(source)); err == nil ||
		!strings.Contains(err.Error(), "load jobs into ROUND_HALF_EVEN columns") {
		t.Errorf("load: error = %v", err)
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
		"t":     {"n": even, "away": away, "s": "", "s.b": even, "i": "", "added": even, "patched": even},
		"own":   {"n": away, "m": even},
		"q":     {"n": even},
		"ctas":  {"n": even},
		"api":   {"n": even},
		"later": {"n": away},
	} {
		if got := modes(table); !reflect.DeepEqual(got, want) {
			t.Errorf("%s rounding modes = %v, want %v", table, got, want)
		}
	}
	mustRun("INSERT t (n, away, added) VALUES (2.25, 2.25, 2.25); INSERT own (n, m) VALUES (2.25, 2.25)")
	got := read(`SELECT 1 AS k, CAST(n AS STRING), CAST(away AS STRING), CAST(added AS STRING) FROM t
		UNION ALL SELECT 2, CAST(n AS STRING), CAST(m AS STRING), NULL FROM own
		UNION ALL SELECT 3, CAST(n AS STRING), NULL, NULL FROM ctas ORDER BY k`)
	want := [][]bigquery.Value{{int64(1), "2.2", "2.3", "2.2"}, {int64(2), "2.3", "2.2", nil}, {int64(3), "2.2", nil, nil}}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("rounded rows = %v, want %v", got, want)
	}
}

// CREATE TABLE LIKE, COPY and CLONE keep the source's default rounding mode, which a column added
// to the copy later takes, unless their OPTIONS set another, and the rounding modes of the source's
// columns. In a dataset with a default, LIKE takes it when the source has none, and COPY and CLONE
// do not; BigQuery's answers to cases that were unverified.
func TestCopiedDefaultRoundingModes(t *testing.T) {
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
// rounding mode, neither the source's nor the dataset's. Copy, query and load jobs that replace a
// table keep its default, which the new fields do not take, and appending keeps it too. BigQuery's
// answers, which bq show reported for the same jobs.
func TestJobDefaultRoundingModes(t *testing.T) {
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
