package goclient

import (
	"context"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// runScript runs sql with dataset as its default dataset and waits for the job to finish.
func runScript(t *testing.T, client *bigquery.Client, dataset, sql string) (*bigquery.Job,
	*bigquery.JobStatus, error) {
	t.Helper()
	ctx := context.Background()
	query := client.Query(sql)
	query.DefaultDatasetID = dataset
	job, err := query.Run(ctx)
	if err != nil {
		return nil, nil, err
	}
	status, err := job.Wait(ctx)
	if err != nil {
		return job, nil, err
	}
	return job, status, status.Err()
}

func TestMultiStatementQueryReturnsItsLastResult(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_scripts")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })

	job, status, err := runScript(t, client, dataset.DatasetID, `
DECLARE n INT64 DEFAULT 3;
CREATE TABLE t (id INT64, tags ARRAY<STRING>);
WHILE n > 0 DO
  INSERT t (id, tags) VALUES (n, ['a']);
  SET n = n - 1;
END WHILE;
SELECT COUNT(*) AS c, SUM(id) AS s FROM t;`)
	if err != nil {
		t.Fatal(err)
	}
	statistics := status.Statistics.Details.(*bigquery.QueryStatistics)
	if statistics.StatementType != "SCRIPT" {
		t.Errorf("got statement type %q, want SCRIPT", statistics.StatementType)
	}
	rows, err := job.Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var row struct{ C, S int64 }
	if err := rows.Next(&row); err != nil {
		t.Fatal(err)
	}
	if row.C != 3 || row.S != 6 {
		t.Errorf("got count %d and sum %d, want 3 and 6", row.C, row.S)
	}
	if err := rows.Next(&row); err != iterator.Done {
		t.Errorf("got another row or an error: %v", err)
	}
}

func TestMultiStatementQueryFailsWithTheFailedStatement(t *testing.T) {
	client := newClient(t)
	_, _, err := runScript(t, client, "", "SELECT 1;\nSELECT 1/0;")
	if err == nil || !strings.Contains(err.Error(), "division by zero") {
		t.Errorf("got %v, want a division by zero", err)
	}
}

func TestMultiStatementQueryFailsWhenAVariableBreaksItsTypeParameters(t *testing.T) {
	client := newClient(t)
	_, _, err := runScript(t, client, "", "DECLARE x STRING(3) DEFAULT 'abcd'; SELECT x")
	if err == nil {
		t.Error("got no error, want STRING(3) to fail to take 'abcd'")
	}
}

// The emulator rejects what it does not run, even inside an exception handler, which would
// otherwise handle the rejection as an error of the script.
func TestMultiStatementQueryRejectsUnsupportedForms(t *testing.T) {
	client := newClient(t)
	for _, sql := range []string{
		"BEGIN EXECUTE IMMEDIATE 'BEGIN SELECT 1; END'; EXCEPTION WHEN ERROR THEN SELECT 1; END",
		"BEGIN EXECUTE IMMEDIATE 'BEGIN TRANSACTION'; EXCEPTION WHEN ERROR THEN SELECT 1; END",
		"CALL d.p(); SELECT 1",
		"BEGIN TRANSACTION; CREATE TEMP FUNCTION f(x INT64) AS (x); ROLLBACK TRANSACTION",
		"BEGIN BEGIN TRANSACTION; SELECT 1 / 0; EXCEPTION WHEN ERROR THEN COMMIT TRANSACTION; END",
		"SET @@time_zone = 'Asia/Tokyo'; SELECT CURRENT_DATE()",
	} {
		_, _, err := runScript(t, client, "", sql)
		if err == nil || !strings.Contains(err.Error(), "The emulator does not support") {
			t.Errorf("%s: got %v, want an unsupported error", sql, err)
		}
	}
}

// EXECUTE IMMEDIATE follows
// https://cloud.google.com/bigquery/docs/reference/standard-sql/procedural-language#execute_immediate,
// whose examples set y to 5 and fill Books with four rows, the earliest from 1599.
func TestExecuteImmediateRunsTheDocumentedExamples(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	job, _, err := runScript(t, client, "", `
DECLARE y INT64;
DECLARE z INT64;
DECLARE book_name STRING DEFAULT 'Ulysses';
DECLARE book_year INT64 DEFAULT 1922;
DECLARE first_date INT64;
EXECUTE IMMEDIATE "SELECT ? * (? + 2)" INTO y USING 1, 3;
EXECUTE IMMEDIATE "SELECT @a * (@b + 2)" INTO z USING 1 as a, 3 as b;
EXECUTE IMMEDIATE
  "CREATE TEMP TABLE Books (title STRING, publish_date INT64)";
EXECUTE IMMEDIATE
  "INSERT INTO Books (title, publish_date) VALUES('Hamlet', 1599)";
EXECUTE IMMEDIATE
  "INSERT INTO Books (title, publish_date) VALUES(?, ?)"
  USING book_name, book_year;
EXECUTE IMMEDIATE
  "INSERT INTO Books (title, publish_date) VALUES(@name, @year)"
  USING 1815 as year, "Emma" as name;
EXECUTE IMMEDIATE
  CONCAT(
    "INSERT INTO Books (title, publish_date)", "VALUES('Middlemarch', 1871)"
  );
EXECUTE IMMEDIATE "SELECT MIN(publish_date) FROM Books LIMIT 1" INTO first_date;
SELECT title, publish_date, y, z, first_date FROM Books ORDER BY publish_date;`)
	if err != nil {
		t.Fatal(err)
	}
	rows, err := job.Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	type book struct {
		Title       string
		PublishDate int64 `bigquery:"publish_date"`
		Y, Z        int64
		FirstDate   int64 `bigquery:"first_date"`
	}
	want := []book{
		{"Hamlet", 1599, 5, 5, 1599},
		{"Emma", 1815, 5, 5, 1599},
		{"Middlemarch", 1871, 5, 5, 1599},
		{"Ulysses", 1922, 5, 5, 1599},
	}
	for _, w := range want {
		var got book
		if err := rows.Next(&got); err != nil {
			t.Fatal(err)
		}
		if got != w {
			t.Errorf("got %+v, want %+v", got, w)
		}
	}
	if err := rows.Next(&book{}); err != iterator.Done {
		t.Errorf("got another row or an error: %v", err)
	}
}

// The behavior the same documentation describes: the statement's result is the result of the
// whole statement, INTO sets every variable to NULL for no rows and fails for more than one, a
// variable may be both in INTO and in USING, and the statement sees no other variables or query
// parameters, nor control statements or another EXECUTE IMMEDIATE.
func TestExecuteImmediateFollowsTheDocumentedRules(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	for _, c := range []struct {
		sql  string
		want bigquery.Value
	}{
		{`EXECUTE IMMEDIATE "SELECT @a * (@b + 2)" USING 1 as a, 3 as b`, int64(5)},
		{`DECLARE x INT64 DEFAULT 1; EXECUTE IMMEDIATE "SELECT 2 FROM UNNEST([])" INTO x; SELECT x`, nil},
		{`DECLARE y INT64 DEFAULT 1; EXECUTE IMMEDIATE "SELECT ? + 1" INTO y USING y; SELECT y`, int64(2)},
	} {
		job, _, err := runScript(t, client, "", c.sql)
		if err != nil {
			t.Errorf("%s: %v", c.sql, err)
			continue
		}
		rows, err := job.Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		var row []bigquery.Value
		if err := rows.Next(&row); err != nil || len(row) != 1 || row[0] != c.want {
			t.Errorf("%s: got %v and %v, want %v", c.sql, row, err, c.want)
		}
	}

	for _, sql := range []string{
		`DECLARE x INT64; EXECUTE IMMEDIATE "SELECT * FROM UNNEST([1, 2])" INTO x`,
		`DECLARE x INT64 DEFAULT 1; EXECUTE IMMEDIATE "SELECT x"`,
		`EXECUTE IMMEDIATE "EXECUTE IMMEDIATE 'SELECT 1'"`,
		`EXECUTE IMMEDIATE "IF TRUE THEN SELECT 1; END IF"`,
	} {
		if _, _, err := runScript(t, client, "", sql); err == nil {
			t.Errorf("%s: succeeded, want an error", sql)
		}
	}
	query := client.Query(`SELECT @p; EXECUTE IMMEDIATE "SELECT @p"`)
	query.Parameters = []bigquery.QueryParameter{{Name: "p", Value: int64(1)}}
	job, err := query.Run(ctx)
	if err == nil {
		var status *bigquery.JobStatus
		if status, err = job.Wait(ctx); err == nil {
			err = status.Err()
		}
	}
	if err == nil {
		t.Error("EXECUTE IMMEDIATE read a query parameter of the request")
	}
}

func TestMultiStatementQueryRejectsJobOptionsOfASingleStatement(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)

	dryRun := client.Query("SELECT 1; SELECT 2")
	dryRun.DryRun = true
	job, err := dryRun.Run(ctx)
	if err == nil {
		err = job.LastStatus().Err()
	}
	if err == nil || !strings.Contains(err.Error(), "The emulator does not support dry runs") {
		t.Errorf("got %v, want dry runs of a multi-statement query to be unsupported", err)
	}

	destination := client.Query("SELECT 1; SELECT 2")
	destination.Dst = client.Dataset("go_scripts_destination").Table("t")
	job, err = destination.Run(ctx)
	if err == nil {
		var status *bigquery.JobStatus
		if status, err = job.Wait(ctx); err == nil {
			err = status.Err()
		}
	}
	if err == nil || !strings.Contains(err.Error(), "multi-statement") {
		t.Errorf("got %v, want a multi-statement query to have no destination table", err)
	}
}

func TestChildJobsOfAMultiStatementQueryAreUnsupported(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	job, _, err := runScript(t, client, "", "SELECT 1; SELECT 2")
	if err != nil {
		t.Fatal(err)
	}
	_, err = job.Children(ctx).Next()
	if err == nil || !strings.Contains(err.Error(), "The emulator does not support child jobs") {
		t.Errorf("got %v, want child jobs to be unsupported", err)
	}
}

// Temporary tables follow
// https://cloud.google.com/bigquery/docs/multi-statement-queries#temporary_tables.
func TestTemporaryTablesLastForTheirMultiStatementQuery(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_temporary_tables")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	if _, _, err := runScript(t, client, dataset.DatasetID,
		"CREATE TABLE t AS SELECT 1 AS a"); err != nil {
		t.Fatal(err)
	}

	// Once the temporary table is dropped, its name refers to the default dataset's table again.
	job, _, err := runScript(t, client, dataset.DatasetID, `
CREATE TEMP TABLE t AS SELECT 2 AS a;
DROP TABLE t;
SELECT a FROM t;`)
	if err != nil {
		t.Fatal(err)
	}
	rows, err := job.Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var row struct{ A int64 }
	if err := rows.Next(&row); err != nil || row.A != 1 {
		t.Errorf("got %d and %v, want the default dataset's row 1", row.A, err)
	}

	// _SESSION names only temporary tables.
	if _, _, err := runScript(t, client, dataset.DatasetID,
		"SELECT 1; SELECT a FROM _SESSION.t"); err == nil {
		t.Error("_SESSION.t read a table that is not temporary")
	}

	// A temporary table is gone once the multi-statement query that created it ends, and the
	// default dataset never lists it.
	if _, _, err := runScript(t, client, dataset.DatasetID,
		"CREATE TEMP TABLE n AS SELECT 1 AS x; SELECT x FROM n"); err != nil {
		t.Fatal(err)
	}
	if _, _, err := runScript(t, client, dataset.DatasetID, "SELECT 1; SELECT x FROM n"); err == nil {
		t.Error("a temporary table outlived its multi-statement query")
	}
	tables := dataset.Tables(ctx)
	var names []string
	for {
		table, err := tables.Next()
		if err == iterator.Done {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		names = append(names, table.TableID)
	}
	if len(names) != 1 || names[0] != "t" {
		t.Errorf("got tables %v, want only t", names)
	}
}

func TestTemporaryTablesRejectedForms(t *testing.T) {
	client := newClient(t)
	for _, sql := range []string{
		// Only a multi-statement query creates temporary tables.
		"CREATE TEMP TABLE n (x INT64)",
		// A temporary table takes no project or dataset qualifier.
		"CREATE TEMP TABLE d.n (x INT64); SELECT 1",
	} {
		if _, _, err := runScript(t, client, "", sql); err == nil {
			t.Errorf("%s: succeeded, want an error", sql)
		}
	}
	// A view would outlive the temporary table it reads.
	sql := "CREATE TEMP TABLE n AS SELECT 1 AS x; CREATE VIEW go_scripts_temporary_view.v AS SELECT x FROM n"
	_, _, err := runScript(t, client, "", sql)
	if err == nil || !strings.Contains(err.Error(), "The emulator does not support") {
		t.Errorf("%s: got %v, want an unsupported error", sql, err)
	}
}

// Temporary functions have query scope, and declarations and calls are case-insensitive.
// https://cloud.google.com/bigquery/docs/user-defined-functions
func TestTemporaryFunctionsHaveQueryScope(t *testing.T) {
	client := newClient(t)
	for _, name := range []string{"Foo", "foo"} {
		if _, _, err := runScript(t, client, "", "CREATE TEMP FUNCTION "+name+"(x INT64) AS (x); SELECT FOO(1)"); err != nil {
			t.Fatal(err)
		}
	}
	if _, _, err := runScript(t, client, "", "SELECT foo(1)"); err == nil {
		t.Error("a temporary function outlived its query")
	}
	_, _, err := runScript(t, client, "", "CREATE TEMP FUNCTION f() AS (1); CREATE TEMP FUNCTION F() AS (2); SELECT f()")
	if err == nil {
		t.Error("duplicate function declarations succeeded")
	}
}

func TestTemporaryFunctionsRejectUnsupportedForms(t *testing.T) {
	client := newClient(t)
	for _, sql := range []string{
		"CREATE FUNCTION d.f(x INT64) AS (x); SELECT 1",
		"@{test_hint = 1} CREATE TEMP FUNCTION f(x INT64) AS (x); SELECT 1",
		"CREATE OR REPLACE TEMP FUNCTION f(x INT64) AS (x); SELECT 1",
		"CREATE TEMP FUNCTION IF NOT EXISTS f(x INT64) AS (x); SELECT 1",
		"CREATE TEMP FUNCTION f(x ANY TYPE) AS (x); SELECT 1",
		"CREATE TEMP FUNCTION f(x FLOAT64) RETURNS FLOAT64 LANGUAGE js AS 'return x'; SELECT 1",
		"BEGIN CREATE TEMP FUNCTION f(x ANY TYPE) AS (x); EXCEPTION WHEN ERROR THEN SELECT 1; END",
		"CREATE TEMP FUNCTION f(x FLOAT64) AS ((SELECT SUM(v) FROM UNNEST([x]) AS v)); SELECT f(RAND())",
		"CREATE TEMP FUNCTION parse_number(x STRING) AS (CAST(x AS INT64)); SELECT SAFE.parse_number('invalid')",
		"CREATE TEMP FUNCTION f(x INT64) OPTIONS (description = 'ignored') AS (x); SELECT 1",
		"CREATE TEMP FUNCTION f(x INT64) AS (x); CREATE VIEW go_udf_views.v AS SELECT ABS(f(1)) AS x",
	} {
		_, _, err := runScript(t, client, "", sql)
		if err == nil || !strings.Contains(err.Error(), "The emulator does not support") {
			t.Errorf("%s: got %v, want an unsupported error", sql, err)
		}
	}
	for _, sql := range []string{
		"CREATE TEMP FUNCTION d.f(x INT64) AS (x); SELECT 1",
		"CREATE TEMP FUNCTION `d.f`(x INT64) AS (x); SELECT 1",
		"CREATE TEMP FUNCTION f(x INT64) AS (x); SELECT f('wrong type')",
		"CREATE TEMP FUNCTION f(x INT64) AS (x); SELECT f(1, 2)",
		"DECLARE x INT64 DEFAULT 1; CREATE TEMP FUNCTION f() AS (x); SELECT f()",
		"CREATE TEMP FUNCTION f(x FLOAT64) AS (1 / x); SELECT f(0)",
		"CREATE TEMP FUNCTION f(x FLOAT64) AS (x); SELECT f(1 / 0)",
	} {
		if _, _, err := runScript(t, client, "", sql); err == nil {
			t.Errorf("%s: succeeded, want an error", sql)
		}
	}
}

// https://cloud.google.com/bigquery/docs/multi-statement-queries#write_a_multi-statement_query
// excludes TEMP function declarations followed by one SELECT from multi-statement queries.
func TestTemporaryFunctionQueryReportsSelectAndResolvedTypes(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	job, status, err := runScript(t, client, "", `
CREATE TEMP FUNCTION AddFourAndDivide(x INT64, y INT64) RETURNS FLOAT64 AS ((x + 4) / y);
SELECT AddFourAndDivide(3, 2) AS answer;`)
	if err != nil {
		t.Fatal(err)
	}
	statistics := status.Statistics.Details.(*bigquery.QueryStatistics)
	if statistics.StatementType != "SELECT" {
		t.Errorf("got statement type %q, want SELECT", statistics.StatementType)
	}
	rows, err := job.Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var row struct{ Answer float64 }
	if err := rows.Next(&row); err != nil {
		t.Fatal(err)
	}
	if len(rows.Schema) != 1 || rows.Schema[0].Type != bigquery.FloatFieldType {
		t.Errorf("got schema %v, want one FLOAT column", rows.Schema)
	}
}

func TestTemporaryFunctionsAcceptNamedQueryParameters(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	query := client.Query(`CREATE TEMP FUNCTION StringIdentity(value STRING) AS (value);
SELECT StringIdentity(@value) AS answer`)
	query.Parameters = []bigquery.QueryParameter{{Name: "value", Value: "a"}}
	rows, err := query.Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var row struct{ Answer string }
	if err := rows.Next(&row); err != nil {
		t.Fatal(err)
	}
	// StringIdentity('a') is recorded in GoogleSQL's call_sql_udf.test.
	if row.Answer != "a" {
		t.Errorf("got %q, want a", row.Answer)
	}
}

// Transaction changes persist only on commit, including when a script exits or fails.
// https://cloud.google.com/bigquery/docs/transactions
func TestTransactionsPersistOnlyCommittedChanges(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_transactions")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	if _, _, err := runScript(t, client, dataset.DatasetID, "CREATE TABLE t AS SELECT 1 AS x"); err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		name, sql string
		fails     bool
		want      int64
	}{
		{"commit", "BEGIN TRANSACTION; UPDATE t SET x = 2 WHERE TRUE; COMMIT TRANSACTION", false, 2},
		{"multiple databases", "CREATE TEMP TABLE tmp (x INT64); BEGIN TRANSACTION; INSERT t VALUES (3); INSERT tmp VALUES (4); COMMIT TRANSACTION", true, 2},
		{"rollback", "BEGIN TRANSACTION; DELETE t WHERE TRUE; ROLLBACK TRANSACTION", false, 2},
		{"unfinished", "BEGIN TRANSACTION; UPDATE t SET x = 3 WHERE TRUE; SELECT x FROM t", false, 2},
		{"return", "BEGIN TRANSACTION; UPDATE t SET x = 3 WHERE TRUE; RETURN", false, 2},
		{"failed", "BEGIN TRANSACTION; UPDATE t SET x = 3 WHERE TRUE; SELECT 1 / 0", true, 2},
		{"permanent DDL", "BEGIN TRANSACTION; CREATE TABLE forbidden (x INT64); COMMIT TRANSACTION", true, 2},
		{"nested", "BEGIN TRANSACTION; BEGIN TRANSACTION; COMMIT TRANSACTION", true, 2},
		{"commit without begin", "SELECT 1; COMMIT TRANSACTION", true, 2},
		{"rollback without begin", "SELECT 1; ROLLBACK TRANSACTION", true, 2},
	} {
		t.Run(tc.name, func(t *testing.T) {
			_, _, err := runScript(t, client, dataset.DatasetID, tc.sql)
			if (err != nil) != tc.fails {
				t.Fatalf("got %v, want failure %v", err, tc.fails)
			}
			job, _, err := runScript(t, client, dataset.DatasetID, "SELECT SUM(x) AS x FROM t")
			if err != nil {
				t.Fatal(err)
			}
			rows, err := job.Read(ctx)
			if err != nil {
				t.Fatal(err)
			}
			var row struct{ X int64 }
			if err := rows.Next(&row); err != nil || row.X != tc.want {
				t.Fatalf("got %d and %v, want %d", row.X, err, tc.want)
			}
		})
	}
}

// The analyzer's logical names are reported even when DuckDB uses positional internal names.
// STRUCT names and order, including duplicate names, follow the data types reference:
// https://cloud.google.com/bigquery/docs/reference/standard-sql/data-types#struct_type
func TestStructResultSchemaPreservesLogicalNames(t *testing.T) {
	client := newClient(t)
	rows, err := client.Query(`SELECT STRUCT(1 AS duplicate, 'x' AS duplicate,
      STRUCT(2 AS _field_2, 'y' AS _field_2) AS nested) AS s`).Read(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	var row []bigquery.Value
	if err := rows.Next(&row); err != nil {
		t.Fatal(err)
	}
	fields := rows.Schema[0].Schema
	if len(fields) != 3 || fields[0].Name != "duplicate" || fields[1].Name != "duplicate" || fields[2].Name != "nested" {
		t.Fatalf("got STRUCT schema %+v", fields)
	}
	inner := fields[2].Schema
	if len(inner) != 2 || inner[0].Name != "_field_2" || inner[1].Name != "_field_2" {
		t.Fatalf("got nested STRUCT schema %+v", inner)
	}
}

// These comparisons would inherit DuckDB's NULL-field equality; keep the newly representable
// types unsupported until comparison semantics are implemented.
func TestAnonymousStructFormsRemainUnsupported(t *testing.T) {
	client := newClient(t)
	for _, sql := range []string{
		`SELECT STRUCT(NULL) = STRUCT(1)`,
		`SELECT STRUCT(1 AS a, NULL AS a) != STRUCT(1 AS a, 2 AS a)`,
		`SELECT NULLIF(STRUCT(NULL), STRUCT(1))`,
		`SELECT CASE STRUCT(NULL) WHEN STRUCT(1) THEN 1 ELSE 2 END`,
		`SELECT STRUCT(NULL) IN (STRUCT(1), STRUCT(2))`,
		`SELECT STRUCT(NULL) IN UNNEST([STRUCT(1), STRUCT(2)])`,
		`SELECT STRUCT(NULL) IN (SELECT STRUCT(1))`,
		`SELECT STRUCT(NULL) = ALL UNNEST([STRUCT(1), STRUCT(2)])`,
		`SELECT [STRUCT(NULL)] = [STRUCT(1)]`,
		`SELECT CAST(STRUCT(1, 2) AS JSON)`,
		`SELECT (SELECT AS STRUCT k, SUM(a) FROM (SELECT 1 AS k) GROUP BY k) FROM UNNEST([1, 2]) AS a`,
	} {
		_, _, err := runScript(t, client, "", sql)
		if err == nil || !strings.Contains(err.Error(), "The emulator does not support") {
			t.Errorf("%s: got %v, want unsupported STRUCT form", sql, err)
		}
	}
}
