package goclient

import (
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

// Source: BigQuery answers to unverified/vector_search.txt (2026-11-22). Every argument but the
// query takes a query parameter as it takes a literal, a NULL top_k or distance_type meaning the
// default.
func TestVectorSearchParameters(t *testing.T) {
	t.Parallel()
	client := newClient(t)
	dataset := newDataset(t, client, "vector_search_parameters")
	queries := datasetQueries{t, client, dataset}
	queries.mustRun("CREATE TABLE twelve AS SELECT [CAST(x AS FLOAT64)] AS e " +
		"FROM UNNEST(GENERATE_ARRAY(1, 12)) AS x")
	queries.mustRun("CREATE TABLE pair AS SELECT [1.0, 2.0] AS e")
	queries.mustRun("CREATE TABLE one AS SELECT [1.0] AS e")
	queries.mustRun("CREATE TABLE strings AS SELECT 'a' AS s, [1.0] AS e")
	queries.mustRun("CREATE TABLE integers AS SELECT 1 AS i, [1.0] AS e")
	queries.mustRun("CREATE TABLE q AS SELECT [1.0] AS f, [1.0, 2.0] AS e")
	for _, c := range []struct {
		name       string
		sql        string
		parameters []bigquery.QueryParameter
		want       bigquery.Value
		wantError  string
	}{
		{
			name:       "top_k",
			sql:        "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE twelve, 'e', query_value => [0.0], top_k => @k)",
			parameters: []bigquery.QueryParameter{{Name: "k", Value: int64(1)}},
			want:       int64(1),
		},
		{
			name: "distance_type",
			sql: "SELECT distance FROM VECTOR_SEARCH(TABLE pair, 'e', query_value => [1.0, 1.0], " +
				"distance_type => @t)",
			parameters: []bigquery.QueryParameter{{Name: "t", Value: "DOT_PRODUCT"}},
			want:       float64(-3),
		},
		{
			name:       "null_top_k",
			sql:        "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE twelve, 'e', query_value => [0.0], top_k => @k)",
			parameters: []bigquery.QueryParameter{{Name: "k", Value: bigquery.NullInt64{}}},
			want:       int64(10),
		},
		{
			name: "lower_case_distance_type",
			sql: "SELECT distance FROM VECTOR_SEARCH(TABLE pair, 'e', query_value => [1.0, 1.0], " +
				"distance_type => @t)",
			parameters: []bigquery.QueryParameter{{Name: "t", Value: "cosine"}},
			want:       0.05131670194948623,
		},
		{
			name: "unknown_distance_type",
			sql: "SELECT distance FROM VECTOR_SEARCH(TABLE pair, 'e', query_value => [1.0, 1.0], " +
				"distance_type => @t)",
			parameters: []bigquery.QueryParameter{{Name: "t", Value: "MANHATTAN"}},
			wantError: "distance_type argument of VECTOR_SEARCH should be one of: EUCLIDEAN, COSINE, " +
				"DOT_PRODUCT. Got MANHATTAN.",
		},
		{
			name: "null_distance_type",
			sql: "SELECT distance FROM VECTOR_SEARCH(TABLE pair, 'e', query_value => [1.0, 1.0], " +
				"distance_type => @t)",
			parameters: []bigquery.QueryParameter{{Name: "t", Value: bigquery.NullString{}}},
			want:       1.0,
		},
		{
			name:       "options",
			sql:        "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE one, 'e', query_value => [1.0], options => @o)",
			parameters: []bigquery.QueryParameter{{Name: "o", Value: "{}"}},
			want:       int64(1),
		},
		{
			name:       "column_to_search",
			sql:        "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE one, @c, query_value => [1.0])",
			parameters: []bigquery.QueryParameter{{Name: "c", Value: "e"}},
			want:       int64(1),
		},
		{
			name:       "query_column_to_search",
			sql:        "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE one, 'e', (SELECT [1.0] AS e), @c)",
			parameters: []bigquery.QueryParameter{{Name: "c", Value: "e"}},
			want:       int64(1),
		},
		{
			name:       "malformed_options",
			sql:        "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE one, 'e', query_value => [1.0], options => @o)",
			parameters: []bigquery.QueryParameter{{Name: "o", Value: `{"x":1}`}},
			wantError:  `Malformated options: {"x":1}`,
		},
		{
			name:       "options_out_of_range",
			sql:        "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE one, 'e', query_value => [1.0], options => @o)",
			parameters: []bigquery.QueryParameter{{Name: "o", Value: `{"fraction_lists_to_search":2}`}},
			wantError:  "fraction_lists_to_search in options of VECTOR_SEARCH should be in the range of (0, 1]. Got 2.",
		},
		{
			name:       "malformed_options_before_dimensions",
			sql:        "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE pair, 'e', query_value => [1.0], options => @o)",
			parameters: []bigquery.QueryParameter{{Name: "o", Value: `{"x":1}`}},
			wantError:  `Malformated options: {"x":1}`,
		},
		{
			name: "malformed_options_before_batch_dimensions",
			sql: "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE pair, 'e', (SELECT [1.0] AS e), " +
				"options => @o)",
			parameters: []bigquery.QueryParameter{{Name: "o", Value: `{"x":1}`}},
			wantError:  `Malformated options: {"x":1}`,
		},
		{
			name:       "null_options",
			sql:        "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE one, 'e', query_value => [1.0], options => @o)",
			parameters: []bigquery.QueryParameter{{Name: "o", Value: bigquery.NullString{}}},
			want:       int64(1),
		},
		{
			name: "malformed_options_without_rows",
			sql: "SELECT COUNT(*) FROM VECTOR_SEARCH((SELECT * FROM one WHERE FALSE), 'e', " +
				"query_value => [1.0], options => @o)",
			parameters: []bigquery.QueryParameter{{Name: "o", Value: `{"x":1}`}},
			wantError:  `Malformated options: {"x":1}`,
		},
		{
			name:       "column_to_search_case",
			sql:        "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE one, @c, query_value => [1.0])",
			parameters: []bigquery.QueryParameter{{Name: "c", Value: "E"}},
			want:       int64(1),
		},
		{
			name:       "missing_column_to_search",
			sql:        "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE one, @c, query_value => [1.0])",
			parameters: []bigquery.QueryParameter{{Name: "c", Value: "x"}},
			wantError:  "Column x should exist in the 1st argument of VECTOR_SEARCH.",
		},
		{
			name:       "string_column_to_search",
			sql:        "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE strings, @c, query_value => [1.0])",
			parameters: []bigquery.QueryParameter{{Name: "c", Value: "s"}},
			wantError:  "No generation expression found for the column_to_search: s.",
		},
		{
			name:       "integer_column_to_search",
			sql:        "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE integers, @c, query_value => [1.0])",
			parameters: []bigquery.QueryParameter{{Name: "c", Value: "i"}},
			wantError: "column_to_search argument of VECTOR_SEARCH: i should be of type ARRAY<FLOAT64>, " +
				"STRING, ObjectRef, or STRUCT<ARRAY<FLOAT64>, STRING>.",
		},
		{
			name:       "null_column_to_search",
			sql:        "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE one, @c, query_value => [1.0])",
			parameters: []bigquery.QueryParameter{{Name: "c", Value: bigquery.NullString{}}},
			wantError:  "2nd argument of VECTOR_SEARCH must be specified.",
		},
		{
			name: "missing_column_to_search_without_rows",
			sql: "SELECT COUNT(*) FROM VECTOR_SEARCH((SELECT * FROM one WHERE FALSE), @c, " +
				"query_value => [1.0])",
			parameters: []bigquery.QueryParameter{{Name: "c", Value: "x"}},
			wantError:  "Column x should exist in the 1st argument of VECTOR_SEARCH.",
		},
		{
			name:       "default_query_column",
			sql:        "SELECT ARRAY_AGG(TO_JSON_STRING(query.e)) FROM VECTOR_SEARCH(TABLE one, @c, TABLE q)",
			parameters: []bigquery.QueryParameter{{Name: "c", Value: "e"}},
			wantError: "Dimension of column `e` in the base table does not match the dimension of " +
				"column `e` in the query data.",
		},
		{
			name:       "column_to_search_dimension",
			sql:        "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE pair, @c, query_value => [1.0])",
			parameters: []bigquery.QueryParameter{{Name: "c", Value: "e"}},
			wantError:  "Array inputs are not equal in length",
		},
		{
			name:       "missing_query_column_to_search",
			sql:        "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE one, 'e', (SELECT [1.0] AS e), @c)",
			parameters: []bigquery.QueryParameter{{Name: "c", Value: "x"}},
			wantError:  "x should exist in 3rd argument of VECTOR_SEARCH.",
		},
		{
			name:       "integer_query_column_to_search",
			sql:        "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE one, 'e', (SELECT [1.0] AS e, 1 AS i), @c)",
			parameters: []bigquery.QueryParameter{{Name: "c", Value: "i"}},
			wantError:  "query_column_to_search argument of VECTOR_SEARCH: i should be of type ARRAY<FLOAT64>.",
		},
		{
			name:       "null_query_column_to_search",
			sql:        "SELECT COUNT(*) FROM VECTOR_SEARCH(TABLE one, 'e', (SELECT [1.0] AS e), @c)",
			parameters: []bigquery.QueryParameter{{Name: "c", Value: bigquery.NullString{}}},
			want:       int64(1),
		},
	} {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			query := client.Query(c.sql)
			query.DefaultDatasetID = dataset.DatasetID
			query.Parameters = c.parameters
			rows, err := query.Read(t.Context())
			if c.wantError != "" {
				if err == nil || !strings.Contains(err.Error(), c.wantError) {
					t.Fatalf("got %v, want error containing %q", err, c.wantError)
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			got := readRows(t, rows)
			if len(got) != 1 || len(got[0]) != 1 || got[0][0] != c.want {
				t.Fatalf("got %v, want [[%v]]", got, c.want)
			}
		})
	}
}
