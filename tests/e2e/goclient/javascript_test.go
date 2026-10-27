package goclient

import "testing"

// Temporary UDFs belong to their query, as
// third_party/bigquery_docs/user-defined-functions.md describes.
// Run separate jobs in order so that a JavaScript global leaking between them changes the result.
func TestJavaScriptUdfKeepsNoStateAcrossQueries(t *testing.T) {
	t.Parallel()
	client := newClient(t)
	const sql = `CREATE TEMP FUNCTION f() RETURNS STRING LANGUAGE js AS """
		globalThis.n = (globalThis.n || 0) + 1;
		return String(globalThis.n);
	""";
	SELECT f()`
	first, err := queryValue(t, client, "", sql)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := first.(string); !ok {
		t.Fatalf("first result has type %T, want STRING", first)
	}
	second, err := queryValue(t, client, "", sql)
	if err != nil {
		t.Fatal(err)
	}
	if first != second {
		t.Errorf("JavaScript state leaked between queries: first result %v, second result %v", first, second)
	}
}
