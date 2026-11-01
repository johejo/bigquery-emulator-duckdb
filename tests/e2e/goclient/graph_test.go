package goclient

import (
	"context"
	"reflect"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// The fixture and answers are the FinGraph examples from BigQuery's graph reference:
// third_party/bigquery_docs/reference/standard-sql/graph-schema-statements.md
// third_party/bigquery_docs/reference/standard-sql/graph-query-statements.md
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
		"CREATE PROPERTY GRAPH go_graph.measured NODE TABLES (go_graph.Person KEY (id) PROPERTIES (MEASURE(SUM(id)) AS total))",
		"CREATE PROPERTY GRAPH go_graph.inferred NODE TABLES (go_graph.Person)",
		"SELECT * FROM GRAPH_TABLE(go_graph.FinGraph MATCH (p:Person) COLUMNS (p.name))",
		"SELECT * FROM GRAPH_TABLE(go_graph.FinGraph MATCH (p:Person) RETURN p)",
		"SELECT * FROM GRAPH_TABLE(go_graph.FinGraph MATCH path = (p:Person) RETURN path)",
		"SELECT * FROM GRAPH_TABLE(go_graph.FinGraph MATCH path = (p:Person) RETURN NODES(path))",
		"SELECT * FROM GRAPH_TABLE(go_graph.FinGraph MATCH (p:Person) RETURN TO_JSON_STRING(p))",
		"SELECT * FROM GRAPH_TABLE(go_graph.FinGraph MATCH (p:Person) RETURN SAFE.SAME(p, p))",
		"SELECT * FROM GRAPH_TABLE(go_graph.FinGraph MATCH (p:Person) RETURN SAFE.ALL_DIFFERENT(p, p))",
		"SELECT * FROM GRAPH_TABLE(go_graph.FinGraph MATCH (p:Person)(q:Person) RETURN q.name)",
		"GRAPH go_graph.FinGraph MATCH path = (p:Person)-[:Owns]->(a:Account) RETURN COUNT(DISTINCT path) AS n",
		"GRAPH go_graph.FinGraph MATCH (p:Person)-[:Owns]->(a:Account) RETURN COUNT(*) OVER (PARTITION BY p) AS n",
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

// A GQL query returns the rows its last RETURN orders. The fixture and answers are the FinGraph
// examples from BigQuery's graph reference, without TIMESTAMP columns and with INT64 amounts:
// third_party/bigquery_docs/reference/standard-sql/graph-query-statements.md
// third_party/bigquery_docs/reference/standard-sql/graph-query-statements.md
func TestGqlQueryOrder(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_gql")
	if err := dataset.Create(ctx, &bigquery.DatasetMetadata{}); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	read := func(sql string) [][]bigquery.Value {
		t.Helper()
		rows, err := client.Query(sql).Read(ctx)
		if err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
		var got [][]bigquery.Value
		for {
			var row []bigquery.Value
			err := rows.Next(&row)
			if err == iterator.Done {
				return got
			}
			if err != nil {
				t.Fatal(err)
			}
			got = append(got, row)
		}
	}
	for _, sql := range []string{
		"CREATE TABLE go_gql.Person (id INT64, name STRING)",
		"CREATE TABLE go_gql.Account (id INT64)",
		"CREATE TABLE go_gql.PersonOwnAccount (id INT64, account_id INT64)",
		"CREATE TABLE go_gql.AccountTransferAccount (id INT64, to_id INT64, amount INT64)",
		"INSERT go_gql.Person VALUES (1, 'Alex'), (2, 'Dana'), (3, 'Lee')",
		"INSERT go_gql.Account VALUES (7), (16), (20)",
		"INSERT go_gql.PersonOwnAccount VALUES (1, 7), (2, 20), (3, 16)",
		"INSERT go_gql.AccountTransferAccount VALUES (7, 16, 300), (7, 16, 100), (16, 20, 300), (20, 7, 500), (20, 16, 200)",
		`CREATE PROPERTY GRAPH go_gql.FinGraph
   NODE TABLES (go_gql.Account KEY (id), go_gql.Person KEY (id))
   EDGE TABLES (
     go_gql.PersonOwnAccount KEY (id, account_id) SOURCE KEY (id) REFERENCES Person (id)
       DESTINATION KEY (account_id) REFERENCES Account (id) LABEL Owns,
     go_gql.AccountTransferAccount KEY (id, to_id, amount) SOURCE KEY (id) REFERENCES Account (id)
       DESTINATION KEY (to_id) REFERENCES Account (id) LABEL Transfers)`,
	} {
		read(sql)
	}
	for _, test := range []struct {
		sql  string
		want [][]bigquery.Value
	}{
		{
			sql: `GRAPH go_gql.FinGraph
   MATCH (p:Person)-[o:Owns]->(a:Account)
   FOR element in ["all","some"] WITH OFFSET
   RETURN p.Id, element as alert_type, offset
   ORDER BY p.Id, element, offset`,
			want: [][]bigquery.Value{
				{int64(1), "all", int64(0)}, {int64(1), "some", int64(1)},
				{int64(2), "all", int64(0)}, {int64(2), "some", int64(1)},
				{int64(3), "all", int64(0)}, {int64(3), "some", int64(1)},
			},
		},
		{
			sql: `GRAPH go_gql.FinGraph
   MATCH (src:Account {id: 7})-[e:Transfers]->{1, 3}(dst:Account)
   RETURN DISTINCT ARRAY_LENGTH(e) AS hops, dst.id AS destination_account_id
   ORDER BY hops, destination_account_id`,
			want: [][]bigquery.Value{
				{int64(1), int64(16)}, {int64(2), int64(20)}, {int64(3), int64(7)}, {int64(3), int64(16)},
			},
		},
		{
			sql: `GRAPH go_gql.FinGraph
   MATCH (:Account)-[:Transfers]->(account:Account)
   RETURN account, COUNT(*) AS num_incoming_transfers
   GROUP BY account
   NEXT
   MATCH (account:Account)<-[:Owns]-(owner:Person)
   RETURN owner.name AS owner_name, num_incoming_transfers
   ORDER BY num_incoming_transfers DESC, owner_name`,
			want: [][]bigquery.Value{{"Lee", int64(3)}, {"Alex", int64(1)}, {"Dana", int64(1)}},
		},
	} {
		if got := read(test.sql); !reflect.DeepEqual(got, test.want) {
			t.Errorf("%s: rows = %v, want %v", test.sql, got, test.want)
		}
	}
}
