package functioncoverage

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestQueryBindsTypes(t *testing.T) {
	f := Function{Name: "ROUND"}
	sql, reason := Query(f, Signature{Arguments: []string{"NUMERIC", "INT64", "STRING"}, Result: "NUMERIC", Samples: []string{"", "1", "'ROUND_HALF_EVEN'"}, LiteralArguments: []int{2}})
	if reason != "" {
		t.Fatal(reason)
	}
	for _, fragment := range []string{"ROUND(a0, a1, 'ROUND_HALF_EVEN')", "CAST(2 AS NUMERIC) AS a0", "CAST(1 AS INT64) AS a1"} {
		if !strings.Contains(sql, fragment) {
			t.Errorf("query %q lacks %q", sql, fragment)
		}
	}
}

func TestMissingSampleDoesNotDropForm(t *testing.T) {
	sql, reason := Query(Function{Name: "F"}, Signature{Arguments: []string{"ARRAY<NO_SAMPLE>"}, Result: "DATE"})
	if sql != "" || reason != "no sample for ARRAY<NO_SAMPLE>" {
		t.Fatalf("sql=%q reason=%q", sql, reason)
	}
}

func TestReadRejectsInvalidCatalog(t *testing.T) {
	for _, data := range []string{
		`{"unexpected":true}`,
		`{"functions":[]} {}`,
		`{"functions":[{"name":"F","source":"https://example.com","signatures":[{"arguments":["INT64"],"result":"INT64"}]}]}`,
		`{"functions":[{"name":"F","source":"third_party/bigquery_docs/reference/standard-sql/mathematical_functions.md","signatures":[]}]}`,
		`{"functions":[{"name":"F","source":"third_party/bigquery_docs/reference/standard-sql/mathematical_functions.md","signatures":[{"arguments":["INT64"],"result":"INT64","literal_arguments":[0]}]}]}`,
		`{"functions":[{"name":"F","source":"third_party/bigquery_docs/reference/standard-sql/mathematical_functions.md","signatures":[{"arguments":["INT64"],"result":"INT64"},{"arguments":["INT64"],"result":"FLOAT64"}]}]}`,
	} {
		path := filepath.Join(t.TempDir(), "catalog.json")
		if err := os.WriteFile(path, []byte(data), 0600); err != nil {
			t.Fatal(err)
		}
		if _, err := Read(path); err == nil {
			t.Errorf("accepted %s", data)
		}
	}
}
