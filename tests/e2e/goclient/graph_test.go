package goclient

import (
	"context"
	"reflect"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// The fixture and answers are the FinGraph examples from BigQuery's graph reference:
// https://cloud.google.com/bigquery/docs/reference/standard-sql/graph-schema-statements#fingraph_example
// https://cloud.google.com/bigquery/docs/reference/standard-sql/graph-query-statements#gql_call
func TestPropertyGraph(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_graph")
	if err := dataset.Create(ctx, &bigquery.DatasetMetadata{}); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	run := func(sql string) *bigquery.RowIterator {
		t.Helper()
		rows, err := client.Query(sql).Read(ctx)
		if err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
		return rows
	}
	for _, sql := range []string{
		"CREATE TABLE go_graph.Person (id INT64, name STRING)",
		"CREATE TABLE go_graph.Account (id INT64)",
		"CREATE TABLE go_graph.PersonOwnAccount (id INT64, account_id INT64)",
		"INSERT go_graph.Person VALUES (1, 'Alex'), (2, 'Dana'), (3, 'Lee')",
		"INSERT go_graph.Account VALUES (7), (16), (20)",
		"INSERT go_graph.PersonOwnAccount VALUES (1, 7), (2, 20), (3, 16)",
		`CREATE PROPERTY GRAPH go_graph.FinGraph
   NODE TABLES (go_graph.Person KEY (id), go_graph.Account KEY (id))
   EDGE TABLES (go_graph.PersonOwnAccount KEY (id, account_id)
     SOURCE KEY (id) REFERENCES Person (id)
     DESTINATION KEY (account_id) REFERENCES Account (id) LABEL Owns)`,
	} {
		run(sql)
	}
	rows := run(`SELECT name, account_id FROM GRAPH_TABLE(go_graph.FinGraph
   MATCH (p:Person)-[:Owns]->(a:Account)
   RETURN p.name AS name, a.id AS account_id) ORDER BY name`)
	var got [][]bigquery.Value
	for {
		var row []bigquery.Value
		err := rows.Next(&row)
		if err == iterator.Done {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		got = append(got, row)
	}
	want := [][]bigquery.Value{{"Alex", int64(7)}, {"Dana", int64(20)}, {"Lee", int64(16)}}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("rows = %v, want %v", got, want)
	}
	if len(rows.Schema) != 2 || rows.Schema[0].Type != bigquery.StringFieldType || rows.Schema[1].Type != bigquery.IntegerFieldType {
		t.Fatalf("schema = %v", rows.Schema)
	}
	for _, sql := range []string{
		"CREATE OR REPLACE PROPERTY GRAPH go_graph.FinGraph NODE TABLES (go_graph.Person KEY (id))",
		"CREATE PROPERTY GRAPH go_graph.computed NODE TABLES (go_graph.Person KEY (id) PROPERTIES (id + 1 AS x))",
		"SELECT * FROM GRAPH_TABLE(go_graph.FinGraph MATCH (p:Person)-[:Owns]->{1,2}(a:Account) RETURN p.name)",
		"SELECT * FROM GRAPH_TABLE(go_graph.FinGraph MATCH (p:Person) COLUMNS (p.name))",
		"SELECT * FROM GRAPH_TABLE(go_graph.FinGraph MATCH (p:Person) RETURN p)",
		"SELECT * FROM GRAPH_TABLE(go_graph.FinGraph MATCH path = (p:Person) RETURN path)",
		"SELECT * FROM GRAPH_TABLE(go_graph.FinGraph MATCH path = (p:Person) RETURN NODES(path))",
		"SELECT * FROM GRAPH_TABLE(go_graph.FinGraph MATCH (p:Person) RETURN TO_JSON_STRING(p))",
	} {
		if _, err := client.Query(sql).Read(ctx); err == nil {
			t.Fatalf("unsupported form succeeded: %s", sql)
		}
	}
	run("DROP PROPERTY GRAPH go_graph.FinGraph")
	if _, err := client.Query("SELECT * FROM GRAPH_TABLE(go_graph.FinGraph MATCH (p:Person) RETURN p.name)").Read(ctx); err == nil {
		t.Fatal("query of dropped graph succeeded")
	}
}
