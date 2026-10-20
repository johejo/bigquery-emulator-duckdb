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

// A GEOGRAPHY parameter is a GEOGRAPHY in the query, given as WKT; the coordinates are those of
// scalars/geography.txt. A value that is not WKT fails the query, and GeoJSON, which BigQuery
// takes, is unsupported.
func TestGeographyQueryParameters(t *testing.T) {
	client := newClient(t)
	read := func(value string) ([]bigquery.Value, error) {
		query := client.Query("SELECT ST_X(@p), ST_Y(@p)")
		query.Parameters = []bigquery.QueryParameter{
			{Name: "p", Value: bigquery.NullGeography{GeographyVal: value, Valid: true}},
		}
		rows, err := query.Read(context.Background())
		if err != nil {
			return nil, err
		}
		var got []bigquery.Value
		return got, rows.Next(&got)
	}
	got, err := read("POINT(-122.35 47.62)")
	if err != nil {
		t.Fatal(err)
	}
	if want := []bigquery.Value{-122.35, 47.62}; !reflect.DeepEqual(got, want) {
		t.Errorf("values = %#v, want %#v", got, want)
	}
	if _, err := read("POINT(1"); err == nil {
		t.Error("query succeeded with invalid WKT")
	}
	if _, err := read(`{"type": "Point", "coordinates": [1, 2]}`); err == nil ||
		!strings.Contains(err.Error(), "does not support GeoJSON") {
		t.Errorf("GeoJSON parameter: %v, want an error that GeoJSON is unsupported", err)
	}
}
