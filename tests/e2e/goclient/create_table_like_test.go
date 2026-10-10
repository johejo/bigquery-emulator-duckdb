package goclient

import (
	"context"
	"math/big"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"cloud.google.com/go/civil"
	"google.golang.org/api/iterator"
)

// CREATE TABLE LIKE copies only the metadata of the source table: its schema, partitioning,
// clustering and options, which the statement's own clauses replace.
// With AS SELECT, the data comes from the query, as documented at:
// https://cloud.google.com/bigquery/docs/reference/standard-sql/data-definition-language#create_table_like
func TestCreateTableLike(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_create_table_like")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	run := func(sql string) error {
		query := client.Query(sql)
		query.DefaultDatasetID = dataset.DatasetID
		job, err := query.Run(ctx)
		if err != nil {
			return err
		}
		status, err := job.Wait(ctx)
		if err != nil {
			return err
		}
		return status.Err()
	}
	for _, sql := range []string{
		`CREATE TABLE source (
		   id INT64 NOT NULL OPTIONS (description = 'key'),
		   day DATE,
		   amount NUMERIC(10, 2),
		   tags ARRAY<STRING>,
		   point STRUCT<x INT64 OPTIONS (description = 'across')>)
		 PARTITION BY day CLUSTER BY id
		 OPTIONS (description = 'the source', friendly_name = 'Source', labels = [('env', 'dev')])`,
		`INSERT source (id, day) VALUES (1, DATE '2024-01-02')`,
		`CREATE TABLE copy LIKE source`,
		`CREATE TABLE custom LIKE source PARTITION BY DATE_TRUNC(day, MONTH) CLUSTER BY day
		 OPTIONS (description = 'custom')`,
		`CREATE TABLE filled LIKE source AS SELECT
		   7 AS query_id, DATE '2024-02-03' AS query_day, NUMERIC '12.34' AS query_amount,
		   ['query'] AS query_tags, STRUCT(9 AS x) AS query_point`,
		`CREATE TABLE filled_custom LIKE source
		 PARTITION BY DATE_TRUNC(day, MONTH) CLUSTER BY day OPTIONS (description = 'custom')
		 AS SELECT * FROM filled`,
		`CREATE TABLE empty LIKE source AS SELECT * FROM source WHERE FALSE`,
		`CREATE TABLE text_source (s STRING(3) OPTIONS (description = 'short'))`,
		`CREATE TABLE nested_required (s STRUCT<x INT64 NOT NULL>)`,
		// IF NOT EXISTS does not evaluate the query or change the existing metadata.
		`CREATE TABLE IF NOT EXISTS filled LIKE source OPTIONS (description = 'ignored')
		 AS SELECT ERROR('must not run'), day, amount, tags, point FROM source`,
		// The new table has no relationship to the source table after creation.
		`ALTER TABLE source ADD COLUMN later STRING`,
		`CREATE VIEW v AS SELECT id FROM source`,
		`CREATE TABLE defaulted (a INT64 DEFAULT 1)`,
	} {
		if err := run(sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	source, err := dataset.Table("source").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}

	copied, err := dataset.Table("copy").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := copied.Schema, source.Schema[:len(source.Schema)-1]; !reflect.DeepEqual(got, want) {
		t.Errorf("schema = %+v, want %+v", got, want)
	}
	if copied.Description != "the source" || copied.Name != "Source" ||
		!reflect.DeepEqual(copied.Labels, map[string]string{"env": "dev"}) {
		t.Errorf("description, name, labels = %q, %q, %v", copied.Description, copied.Name, copied.Labels)
	}
	if got, want := copied.TimePartitioning, (&bigquery.TimePartitioning{Type: bigquery.DayPartitioningType, Field: "day"}); !reflect.DeepEqual(got, want) {
		t.Errorf("time partitioning = %+v, want %+v", got, want)
	}
	if got, want := copied.Clustering, (&bigquery.Clustering{Fields: []string{"id"}}); !reflect.DeepEqual(got, want) {
		t.Errorf("clustering = %+v, want %+v", got, want)
	}
	rows := dataset.Table("copy").Read(ctx)
	var row []bigquery.Value
	if err := rows.Next(&row); err != iterator.Done {
		t.Errorf("copy has a row %v, err %v", row, err)
	}
	// NOT NULL is copied with the schema.
	if err := run("INSERT copy (day) VALUES (DATE '2024-01-02')"); err == nil {
		t.Error("inserting NULL into a REQUIRED column of the copy succeeded")
	}

	custom, err := dataset.Table("custom").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if custom.Description != "custom" || custom.Name != "Source" ||
		!reflect.DeepEqual(custom.Labels, map[string]string{"env": "dev"}) {
		t.Errorf("description, name, labels = %q, %q, %v", custom.Description, custom.Name, custom.Labels)
	}
	if got, want := custom.TimePartitioning, (&bigquery.TimePartitioning{Type: bigquery.MonthPartitioningType, Field: "day"}); !reflect.DeepEqual(got, want) {
		t.Errorf("time partitioning = %+v, want %+v", got, want)
	}
	if got, want := custom.Clustering, (&bigquery.Clustering{Fields: []string{"day"}}); !reflect.DeepEqual(got, want) {
		t.Errorf("clustering = %+v, want %+v", got, want)
	}

	for _, tc := range []struct {
		name string
		want *bigquery.TableMetadata
	}{
		{"filled", copied},
		{"filled_custom", custom},
	} {
		got, err := dataset.Table(tc.name).Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(got.Schema, tc.want.Schema) || got.Description != tc.want.Description ||
			got.Name != tc.want.Name || !reflect.DeepEqual(got.Labels, tc.want.Labels) ||
			!reflect.DeepEqual(got.TimePartitioning, tc.want.TimePartitioning) ||
			!reflect.DeepEqual(got.Clustering, tc.want.Clustering) {
			t.Errorf("%s: inherited metadata = %+v, want %+v", tc.name, got, tc.want)
		}
		rows := dataset.Table(tc.name).Read(ctx)
		var row struct {
			ID     int64
			Day    civil.Date
			Amount *big.Rat
			Tags   []string
			Point  struct{ X int64 }
		}
		if err := rows.Next(&row); err != nil {
			t.Fatal(err)
		}
		if row.ID != 7 || row.Day != (civil.Date{Year: 2024, Month: 2, Day: 3}) ||
			row.Amount == nil || row.Amount.Cmp(big.NewRat(1234, 100)) != 0 ||
			!reflect.DeepEqual(row.Tags, []string{"query"}) || row.Point.X != 9 {
			t.Errorf("%s: row = %+v, want {7 2024-02-03 12.34 [query] {9}}", tc.name, row)
		}
		if err := rows.Next(&row); err != iterator.Done {
			t.Errorf("%s: extra row = %+v, err %v", tc.name, row, err)
		}
	}
	// Inherited constraints apply both to the query and to later writes. A failed replacement
	// leaves the previous table intact, including its rows and metadata.
	for _, sql := range []string{
		`INSERT filled (day) VALUES (DATE '2024-01-02')`,
		`CREATE TABLE invalid LIKE copy AS SELECT NULL, day, amount, tags, point FROM filled`,
		`CREATE OR REPLACE TABLE filled LIKE copy AS SELECT NULL, day, amount, tags, point FROM filled`,
		`CREATE TABLE invalid LIKE copy AS SELECT id, day, NUMERIC '100000000', tags, point FROM filled`,
		`CREATE TABLE invalid LIKE copy AS SELECT id FROM filled`,
		`CREATE TABLE invalid LIKE copy AS SELECT 'wrong type', day, amount, tags, point FROM filled`,
	} {
		if err := run(sql); err == nil {
			t.Errorf("%s: succeeded, want an error", sql)
		}
	}
	if _, err := dataset.Table("invalid").Metadata(ctx); err == nil {
		t.Error("failed AS SELECT left a table behind")
	}
	if empty, err := dataset.Table("empty").Metadata(ctx); err != nil {
		t.Fatal(err)
	} else if empty.NumRows != 0 || !reflect.DeepEqual(empty.Schema, copied.Schema) {
		t.Errorf("empty query did not preserve schema: %+v", empty)
	}
	if err := run(`CREATE OR REPLACE TABLE filled LIKE filled AS SELECT * FROM filled`); err != nil {
		t.Fatal(err)
	}
	filled, err := dataset.Table("filled").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if filled.NumRows != 1 || filled.Description != copied.Description || !reflect.DeepEqual(filled.Schema, copied.Schema) {
		t.Errorf("replacement did not preserve rows and metadata: %+v", filled)
	}

	for sql, want := range map[string]string{
		"CREATE TABLE n LIKE nested_required AS SELECT STRUCT(1 AS x) AS s": "CREATE TABLE LIKE AS SELECT with nested NOT NULL",
		"CREATE TABLE n LIKE text_source AS SELECT 'abc' AS s":              "CREATE TABLE LIKE AS SELECT with length parameters",
		"CREATE TABLE n LIKE v":                          "CREATE TABLE LIKE a view",
		"CREATE TABLE n LIKE v AS SELECT id FROM v":      "CREATE TABLE LIKE a view",
		"CREATE TABLE n LIKE defaulted":                  "CREATE TABLE LIKE a table with column defaults",
		"CREATE TABLE n LIKE defaulted AS SELECT 1 AS a": "CREATE TABLE LIKE a table with column defaults",
	} {
		if err := run(sql); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: error = %v, want one containing %q", sql, err, want)
		}
	}
}
