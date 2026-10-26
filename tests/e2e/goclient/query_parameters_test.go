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

// Parameters substitute typed expressions, including arrays and structs:
// https://cloud.google.com/bigquery/docs/parameterized-queries
// NULL parameters are documented by the bq --parameter flag:
// https://cloud.google.com/bigquery/docs/reference/bq-cli-reference#bq_query
func TestQueryParameterValuesAndSchema(t *testing.T) {
	t.Parallel()
	client := newClient(t)
	for _, c := range []struct {
		name   string
		value  any
		want   bigquery.Value
		schema *bigquery.FieldSchema
	}{
		{"integer", int64(42), int64(42), &bigquery.FieldSchema{Type: bigquery.IntegerFieldType}},
		{"float", 1.5, 1.5, &bigquery.FieldSchema{Type: bigquery.FloatFieldType}},
		{"boolean", true, true, &bigquery.FieldSchema{Type: bigquery.BooleanFieldType}},
		{"quoted_string", "it's", "it's", &bigquery.FieldSchema{Type: bigquery.StringFieldType}},
		{"date", civil.Date{Year: 2024, Month: 1, Day: 2}, civil.Date{Year: 2024, Month: 1, Day: 2},
			&bigquery.FieldSchema{Type: bigquery.DateFieldType}},
		{"numeric", big.NewRat(5, 4), big.NewRat(5, 4), &bigquery.FieldSchema{Type: bigquery.NumericFieldType}},
		{"bytes", []byte("abc"), []byte("abc"), &bigquery.FieldSchema{Type: bigquery.BytesFieldType}},
		{"null_string", bigquery.NullString{}, nil, &bigquery.FieldSchema{Type: bigquery.StringFieldType}},
		{"null_integer", bigquery.NullInt64{}, nil, &bigquery.FieldSchema{Type: bigquery.IntegerFieldType}},
		{"null_interval", &bigquery.QueryParameterValue{
			Type: bigquery.StandardSQLDataType{TypeKind: "INTERVAL"}, Value: bigquery.NullString{},
		}, nil, &bigquery.FieldSchema{Type: bigquery.IntervalFieldType}},
		{"geography", bigquery.NullGeography{GeographyVal: "POINT(-122.35 47.62)", Valid: true},
			"POINT(-122.35 47.62)", &bigquery.FieldSchema{Type: bigquery.GeographyFieldType}},
		{"full_geography", bigquery.NullGeography{GeographyVal: "fullglobe", Valid: true},
			fullGeographyWkt, &bigquery.FieldSchema{Type: bigquery.GeographyFieldType}},
		{"full_geography_wkt", bigquery.NullGeography{GeographyVal: fullGeographyWkt, Valid: true},
			fullGeographyWkt, &bigquery.FieldSchema{Type: bigquery.GeographyFieldType}},
		{"null_geography", bigquery.NullGeography{}, nil,
			&bigquery.FieldSchema{Type: bigquery.GeographyFieldType}},
		{"geography_array", []bigquery.NullGeography{{GeographyVal: "LINESTRING(0 0, 132 0)", Valid: true}},
			[]bigquery.Value{"LINESTRING(0 0, 132 0)"},
			&bigquery.FieldSchema{Type: bigquery.GeographyFieldType, Repeated: true}},
		{"integer_array", []int64{1, 2}, []bigquery.Value{int64(1), int64(2)},
			&bigquery.FieldSchema{Type: bigquery.IntegerFieldType, Repeated: true}},
		// The Go client's convertRepeatedRecord returns a nil slice for an empty array.
		{"empty_string_array", []string{}, []bigquery.Value(nil),
			&bigquery.FieldSchema{Type: bigquery.StringFieldType, Repeated: true}},
		{"struct", struct {
			X int64  `bigquery:"x"`
			Y string `bigquery:"y"`
		}{1, "a"}, []bigquery.Value{int64(1), "a"},
			&bigquery.FieldSchema{Type: bigquery.RecordFieldType, Schema: bigquery.Schema{
				{Name: "x", Type: bigquery.IntegerFieldType},
				{Name: "y", Type: bigquery.StringFieldType},
			}}},
	} {
		t.Run(c.name, func(t *testing.T) {
			query := client.Query("SELECT @p AS value")
			query.Parameters = []bigquery.QueryParameter{{Name: "p", Value: c.value}}
			rows, err := query.Read(context.Background())
			if err != nil {
				t.Fatal(err)
			}
			var got []bigquery.Value
			if err := rows.Next(&got); err != nil {
				t.Fatal(err)
			}
			if want := []bigquery.Value{c.want}; !reflect.DeepEqual(got, want) {
				t.Errorf("values = %#v, want %#v", got, want)
			}
			c.schema.Name = "value"
			if want := (bigquery.Schema{c.schema}); !reflect.DeepEqual(rows.Schema, want) {
				t.Errorf("schema = %#v, want %#v", rows.Schema, want)
			}
			if err := rows.Next(&got); err != iterator.Done {
				t.Fatalf("next = %v, want iterator.Done", err)
			}
		})
	}
}

func TestQueryParameterNamesAreCaseInsensitive(t *testing.T) {
	t.Parallel()
	query := newClient(t).Query("SELECT @id AS lower, @ID AS upper")
	query.Parameters = []bigquery.QueryParameter{{Name: "Id", Value: int64(1)}}
	rows, err := query.Read(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	var got []bigquery.Value
	if err := rows.Next(&got); err != nil {
		t.Fatal(err)
	}
	if want := []bigquery.Value{int64(1), int64(1)}; !reflect.DeepEqual(got, want) {
		t.Fatalf("values = %#v, want %#v", got, want)
	}
}

func TestQueryRejectsUndeclaredParameters(t *testing.T) {
	t.Parallel()
	client := newClient(t)
	for _, c := range []struct {
		name       string
		sql        string
		parameters []bigquery.QueryParameter
	}{
		{"named", "SELECT @missing", nil},
		{"positional", "SELECT ?", nil},
		{"missing_position", "SELECT ?, ?, ?", []bigquery.QueryParameter{{Value: int64(1)}, {Value: int64(2)}}},
	} {
		t.Run(c.name, func(t *testing.T) {
			query := client.Query(c.sql)
			query.Parameters = c.parameters
			if _, err := query.Read(context.Background()); err == nil {
				t.Fatal("query succeeded with an undeclared parameter")
			}
		})
	}
}

// A GEOGRAPHY parameter is a GEOGRAPHY in the query, given as WKT whose polygons are oriented,
// at any depth; the coordinates are those of scalars/geography.txt. BigQuery answered the other
// cases, including the errors, which name the parameter even for an element or a field.
func TestGeographyQueryParameters(t *testing.T) {
	t.Parallel()
	client := newClient(t)
	geography := func(value string) bigquery.NullGeography {
		return bigquery.NullGeography{GeographyVal: value, Valid: true}
	}
	clockwise := "POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))"
	for _, c := range []struct {
		name  string
		sql   string
		value any
		want  []bigquery.Value
		err   string
	}{
		{"point", "SELECT ST_X(@p), ST_Y(@p)", geography("POINT(-122.35 47.62)"),
			[]bigquery.Value{-122.35, 47.62}, ""},
		{"case_and_spaces", "SELECT ST_ASTEXT(@p)", geography(" point ( 1  2 ) "),
			[]bigquery.Value{"POINT(1 2)"}, ""},
		{"counterclockwise", "SELECT ST_AREA(@p) < 1e11", geography("POLYGON((0 0, 1 0, 1 1, 0 1, 0 0))"),
			[]bigquery.Value{true}, ""},
		{"clockwise", "SELECT ST_AREA(@p) < 1e11", geography(clockwise),
			[]bigquery.Value{false}, ""},
		{"clockwise_element", "SELECT ST_AREA(@p[OFFSET(0)]) < 1e11",
			[]bigquery.NullGeography{geography(clockwise)}, []bigquery.Value{false}, ""},
		{"invalid", "SELECT ST_X(@p)", geography("POINT(1"), nil,
			"Unparseable query parameter `p` in type `TYPE_GEOGRAPHY`, Expected latitude, but saw " +
				"end of data at position 7. value: 'POINT(1'"},
		{"empty", "SELECT ST_X(@p)", geography(""), nil,
			"Unparseable query parameter `p` in type `TYPE_GEOGRAPHY`, Unexpected end of data at " +
				"position 0. value: ''"},
		{"geojson", "SELECT ST_ASTEXT(@p)", geography(`{"type": "Point", "coordinates": [1, 2]}`), nil,
			"Unparseable query parameter `p` in type `TYPE_GEOGRAPHY`, Unexpected '{' at position 0. " +
				`value: '{"type": "Point", "coordinates": [1, 2]}'`},
		{"wkb", "SELECT ST_ASTEXT(@p)", geography("0101000000000000000000F03F0000000000000040"), nil,
			"Unparseable query parameter `p` in type `TYPE_GEOGRAPHY`, Unexpected " +
				"'0101000000000000000000' at position 0. value: '0101000000000000000000F03F0000000000000040'"},
		{"invalid_element", "SELECT ARRAY_LENGTH(@p)",
			[]bigquery.NullGeography{geography("POINT(1 2)"), geography("POINT(1")}, nil,
			"Unparseable query parameter `p` in type `TYPE_GEOGRAPHY`, Expected latitude, but saw " +
				"end of data at position 7. value: 'POINT(1'"},
		{"invalid_field", "SELECT ST_X(@p.g)", struct {
			G bigquery.NullGeography `bigquery:"g"`
		}{geography("POINT(1")}, nil,
			"Unparseable query parameter `p` in type `TYPE_GEOGRAPHY`, Expected latitude, but saw " +
				"end of data at position 7. value: 'POINT(1'"},
	} {
		t.Run(c.name, func(t *testing.T) {
			query := client.Query(c.sql)
			query.Parameters = []bigquery.QueryParameter{{Name: "p", Value: c.value}}
			rows, err := query.Read(context.Background())
			var got []bigquery.Value
			if err == nil {
				err = rows.Next(&got)
			}
			if c.err != "" {
				if err == nil || !strings.Contains(err.Error(), c.err) {
					t.Fatalf("error = %v, want one containing %q", err, c.err)
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			if !reflect.DeepEqual(got, c.want) {
				t.Errorf("values = %#v, want %#v", got, c.want)
			}
		})
	}
}
