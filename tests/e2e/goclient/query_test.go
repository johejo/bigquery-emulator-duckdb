package goclient

import (
	"context"
	"reflect"
	"strings"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

func TestQueryWithNamedParameters(t *testing.T) {
	ctx := context.Background()
	created := time.Date(2024, 1, 2, 3, 4, 5, 0, time.UTC)

	query := newClient(t).Query(
		"SELECT @id AS id, @name AS name, @created AS created, @tags AS tags")
	query.Parameters = []bigquery.QueryParameter{
		{Name: "id", Value: int64(7)},
		{Name: "name", Value: "alice"},
		{Name: "created", Value: created},
		{Name: "tags", Value: []string{"a", "b"}},
	}
	rows, err := query.Read(ctx)
	if err != nil {
		t.Fatalf("Read: %v", err)
	}

	var row struct {
		ID      int64
		Name    string
		Created time.Time
		Tags    []string
	}
	if err := rows.Next(&row); err != nil {
		t.Fatalf("Next: %v", err)
	}
	if row.ID != 7 || row.Name != "alice" {
		t.Errorf("got id %d and name %q, want 7 and \"alice\"", row.ID, row.Name)
	}
	if !row.Created.Equal(created) {
		t.Errorf("got created %s, want %s", row.Created, created)
	}
	if len(row.Tags) != 2 || row.Tags[0] != "a" || row.Tags[1] != "b" {
		t.Errorf("got tags %q, want [a b]", row.Tags)
	}
	wantSchema := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		{Name: "name", Type: bigquery.StringFieldType},
		{Name: "created", Type: bigquery.TimestampFieldType},
		{Name: "tags", Type: bigquery.StringFieldType, Repeated: true},
	}
	if !reflect.DeepEqual(rows.Schema, wantSchema) {
		t.Errorf("schema = %#v, want %#v", rows.Schema, wantSchema)
	}
}

func TestQueryWithPositionalParameters(t *testing.T) {
	ctx := context.Background()
	query := newClient(t).Query("SELECT ? AS a, ? AS b")
	query.Parameters = []bigquery.QueryParameter{{Value: int64(1)}, {Value: "b"}}
	rows, err := query.Read(ctx)
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	var row []bigquery.Value
	if err := rows.Next(&row); err != nil {
		t.Fatalf("Next: %v", err)
	}
	if len(row) != 2 || row[0] != int64(1) || row[1] != "b" {
		t.Errorf("got row %v, want [1 b]", row)
	}
	wantSchema := bigquery.Schema{
		{Name: "a", Type: bigquery.IntegerFieldType},
		{Name: "b", Type: bigquery.StringFieldType},
	}
	if !reflect.DeepEqual(rows.Schema, wantSchema) {
		t.Errorf("schema = %#v, want %#v", rows.Schema, wantSchema)
	}
}

func TestDryRunReportsTheSchemaWithoutRunning(t *testing.T) {
	ctx := context.Background()
	query := newClient(t).Query("SELECT 1 AS n, 'x' AS s")
	query.DryRun = true

	job, err := query.Run(ctx)
	if err != nil {
		t.Fatalf("Run: %v", err)
	}
	statistics, ok := job.LastStatus().Statistics.Details.(*bigquery.QueryStatistics)
	if !ok {
		t.Fatalf("got statistics %T, want query statistics", job.LastStatus().Statistics.Details)
	}
	if len(statistics.Schema) != 2 || statistics.Schema[0].Name != "n" {
		t.Errorf("got schema %v, want the two columns of the query", statistics.Schema)
	}
}

// TIMESTAMP and TIME hold microseconds, so a literal with more than six fractional digits is
// invalid, as in BigQuery, rather than truncated.
func TestSubmicrosecondLiterals(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	for sql, want := range map[string]string{
		"SELECT TIMESTAMP '2020-01-01 00:00:00.123456789'": "Invalid TIMESTAMP literal",
		"SELECT TIME '01:02:03.123456789'":                 "Invalid TIME literal",
	} {
		if _, err := client.Query(sql).Read(ctx); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: got error %v, want %q", sql, err, want)
		}
	}
}

// A query that fails while it runs is an error from Read, whether the client gets its results
// from jobs.query or from jobs.getQueryResults, rather than an empty result.
func TestQueryRuntimeError(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	query := client.Query("SELECT ERROR('boom')")
	if _, err := query.Read(ctx); err == nil || !strings.Contains(err.Error(), "boom") {
		t.Errorf("Query.Read: got error %v, want boom", err)
	}
	job, err := query.Run(ctx)
	if err != nil {
		t.Fatalf("Run: %v", err)
	}
	if _, err := job.Read(ctx); err == nil || !strings.Contains(err.Error(), "boom") {
		t.Errorf("Job.Read: got error %v, want boom", err)
	}
}

// Result columns take the names and types GoogleSQL resolves, rather than DuckDB's: a column
// without a name is f0_, f1_ and so on, a duplicate column after the first is renamed, and a
// value table of structs comes back as their fields. A dry run reports the same schema.
// https://cloud.google.com/bigquery/docs/reference/standard-sql/query-syntax#implicit_aliases
// https://cloud.google.com/bigquery/docs/reference/standard-sql/query-syntax#duplicate_aliases
func TestResultSchema(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	describe := func(schema bigquery.Schema) string {
		fields := make([]string, len(schema))
		for i, field := range schema {
			fields[i] = field.Name + " " + string(field.Type)
		}
		return strings.Join(fields, ", ")
	}
	for _, c := range []struct {
		sql        string
		parameters []bigquery.QueryParameter
		schema     string
		rows       [][]bigquery.Value
	}{
		// DuckDB sums integers into a HUGEINT, which alone would be reported as BIGNUMERIC.
		{
			sql:    `SELECT SUM(x) AS s, ANY_VALUE('a'), 2.5, NULL, b'\x00\xff' FROM UNNEST([1, 2]) AS x`,
			schema: "s INTEGER, f0_ STRING, f1_ FLOAT, f2_ INTEGER, f3_ BYTES",
			rows:   [][]bigquery.Value{{int64(3), "a", 2.5, nil, []byte{0, 0xff}}},
		},
		{
			sql:    "SELECT 1 AS a, 2 AS a",
			schema: "a INTEGER, a_1 INTEGER",
			rows:   [][]bigquery.Value{{int64(1), int64(2)}},
		},
		// Names that differ only in case are duplicates too, as a dry run on BigQuery reports.
		{
			sql:    "SELECT 1 AS a, 2 AS A",
			schema: "a INTEGER, A_1 INTEGER",
			rows:   [][]bigquery.Value{{int64(1), int64(2)}},
		},
		{
			sql:    "SELECT AS STRUCT 1 AS a, 'x' AS b",
			schema: "a INTEGER, b STRING",
			rows:   [][]bigquery.Value{{int64(1), "x"}},
		},
		// BigQuery names a repeated field of a value table by its position, ignoring case, as a dry
		// run on BigQuery reports.
		{
			sql:    "SELECT AS STRUCT 1 AS a, 2 AS a",
			schema: "a INTEGER, _field_2 INTEGER",
			rows:   [][]bigquery.Value{{int64(1), int64(2)}},
		},
		{
			sql:    "SELECT AS VALUE STRUCT(1 AS a, 2 AS a)",
			schema: "a INTEGER, _field_2 INTEGER",
			rows:   [][]bigquery.Value{{int64(1), int64(2)}},
		},
		{
			sql:    "SELECT AS STRUCT 1 AS a, 2 AS A",
			schema: "a INTEGER, _field_2 INTEGER",
			rows:   [][]bigquery.Value{{int64(1), int64(2)}},
		},
		{
			sql: "SELECT BYTE_LENGTH(@s), BYTE_LENGTH(@b) AS bytes, SAFE_CAST(@s AS INT64)",
			parameters: []bigquery.QueryParameter{
				{Name: "s", Value: "あ"}, {Name: "b", Value: []byte{0, 0xff}},
			},
			schema: "f0_ INTEGER, bytes INTEGER, f1_ INTEGER",
			rows:   [][]bigquery.Value{{int64(3), int64(2), nil}},
		},
		{
			sql:        "SELECT @n * 2 AS x",
			parameters: []bigquery.QueryParameter{{Name: "n", Value: int64(2)}},
			schema:     "x INTEGER",
			rows:       [][]bigquery.Value{{int64(4)}},
		},
		{
			sql: "SELECT x + @min AS y FROM UNNEST([3, 1, 2]) AS x WHERE x > @min " +
				"ORDER BY x DESC LIMIT @n OFFSET @skip",
			parameters: []bigquery.QueryParameter{
				{Name: "min", Value: int64(1)}, {Name: "n", Value: int64(1)}, {Name: "skip", Value: int64(1)},
			},
			schema: "y INTEGER",
			rows:   [][]bigquery.Value{{int64(3)}},
		},
	} {
		query := client.Query(c.sql)
		query.Parameters = c.parameters
		it, err := query.Read(ctx)
		if err != nil {
			t.Errorf("%s: %v", c.sql, err)
			continue
		}
		var rows [][]bigquery.Value
		for {
			var row []bigquery.Value
			err := it.Next(&row)
			if err == iterator.Done {
				break
			}
			if err != nil {
				t.Fatalf("%s: %v", c.sql, err)
			}
			rows = append(rows, row)
		}
		if got := describe(it.Schema); got != c.schema {
			t.Errorf("%s: got schema %s, want %s", c.sql, got, c.schema)
		}
		if !reflect.DeepEqual(rows, c.rows) {
			t.Errorf("%s: got rows %v, want %v", c.sql, rows, c.rows)
		}

		query.DryRun = true
		job, err := query.Run(ctx)
		if err != nil {
			t.Errorf("%s: dry run: %v", c.sql, err)
			continue
		}
		statistics := job.LastStatus().Statistics.Details.(*bigquery.QueryStatistics)
		if got := describe(statistics.Schema); got != c.schema {
			t.Errorf("%s: got dry-run schema %s, want %s", c.sql, got, c.schema)
		}
	}
}

// A parameter keeps the type it is declared with: BigQuery does not coerce a STRING to a number,
// and a cast of one that is not a number fails.
func TestQueryParameterTypes(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	for sql, value := range map[string]string{"SELECT @s * 2": "2", "SELECT CAST(@s AS INT64)": "あ"} {
		query := client.Query(sql)
		query.Parameters = []bigquery.QueryParameter{{Name: "s", Value: value}}
		if _, err := query.Read(ctx); err == nil {
			t.Errorf("%s: succeeded with @s = %q", sql, value)
		}
	}
}
