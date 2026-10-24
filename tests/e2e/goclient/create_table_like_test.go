package goclient

import (
	"context"
	"math/big"
	"reflect"
	"testing"

	"cloud.google.com/go/civil"
	"google.golang.org/api/iterator"
)

// REST metadata/constraints/defaults are covered by create_table_like.api.yaml.
// Keep Table.Read decoding of DATE, NUMERIC, arrays and records into Go structs.
func TestCreateTableLikeTypedRows(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_like_typed_rows")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := creationPaths{t: t, client: client, dataset: dataset}
	p.mustRun(`CREATE TABLE source (id INT64 NOT NULL, day DATE, amount NUMERIC(10,2),tags ARRAY<STRING>,point STRUCT<x INT64>);
 CREATE TABLE filled LIKE source AS SELECT 7,DATE '2024-02-03',NUMERIC '12.34',['query'],STRUCT(9);
 CREATE TABLE filled_custom LIKE source PARTITION BY DATE_TRUNC(day,MONTH) CLUSTER BY day AS SELECT * FROM filled`)
	for _, name := range []string{"filled", "filled_custom"} {
		rows := dataset.Table(name).Read(ctx)
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
		if row.ID != 7 || row.Day != (civil.Date{Year: 2024, Month: 2, Day: 3}) || row.Amount == nil || row.Amount.Cmp(big.NewRat(1234, 100)) != 0 || !reflect.DeepEqual(row.Tags, []string{"query"}) || row.Point.X != 9 {
			t.Fatalf("%s: row=%+v", name, row)
		}
		if err := rows.Next(&row); err != iterator.Done {
			t.Fatalf("extra row: %v", err)
		}
	}
}
