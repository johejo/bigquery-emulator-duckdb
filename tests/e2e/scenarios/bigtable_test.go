package scenarios

import (
	"context"
	"os"
	"testing"
	"time"

	adminpb "cloud.google.com/go/bigtable/admin/apiv2/adminpb"
	btpb "cloud.google.com/go/bigtable/apiv2/bigtablepb"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

func createBigtableFixture(t *testing.T, project, tableID string, families map[string]*adminpb.ColumnFamily) (btpb.BigtableClient, string) {
	t.Helper()
	host := os.Getenv("BIGTABLE_EMULATOR_HOST")
	if host == "" {
		t.Fatal("BIGTABLE_EMULATOR_HOST is required; run just e2e")
	}
	conn, err := grpc.NewClient(host, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = conn.Close() })
	ctx := t.Context()
	admin := adminpb.NewBigtableTableAdminClient(conn)
	parent := "projects/" + project + "/instances/bqanswers"
	table := parent + "/tables/" + tableID
	if _, err := admin.CreateTable(ctx, &adminpb.CreateTableRequest{
		Parent: parent, TableId: tableID,
		Table: &adminpb.Table{ColumnFamilies: families},
	}); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		ctx, cancel := context.WithTimeout(context.WithoutCancel(ctx), 5*time.Second)
		defer cancel()
		if _, err := admin.DeleteTable(ctx, &adminpb.DeleteTableRequest{Name: table}); err != nil {
			t.Error(err)
		}
	})
	return btpb.NewBigtableClient(conn), table
}

func bigtableCell(family, qualifier, value string, micros int64) *btpb.Mutation {
	return &btpb.Mutation{Mutation: &btpb.Mutation_SetCell_{SetCell: &btpb.Mutation_SetCell{
		FamilyName: family, ColumnQualifier: []byte(qualifier), Value: []byte(value), TimestampMicros: micros,
	}}}
}

// Mirrors the fixture described in bigtable_qualifiers.api.yaml.
func seedBigtableQualifiers(t *testing.T, project string) {
	t.Helper()
	data, table := createBigtableFixture(t, project, "bigtable-qualifier-probe", map[string]*adminpb.ColumnFamily{"family_a": {}, "family_b": {}})
	ctx := t.Context()
	for _, row := range []struct {
		key   string
		cells []*btpb.Mutation
	}{
		{"both", []*btpb.Mutation{
			bigtableCell("family_a", "promoted", "old", 1000), bigtableCell("family_a", "promoted", "new", 2000),
			bigtableCell("family_a", "loose", "unlisted", 2000),
			bigtableCell("family_a", "\xff", "raw-old", 1000), bigtableCell("family_a", "\xff", "raw-new", 2000),
		}},
		{"fallback_only", []*btpb.Mutation{bigtableCell("family_a", "loose", "unlisted", 2000)}},
		{"named_only", []*btpb.Mutation{bigtableCell("family_a", "promoted", "solo", 2000)}},
		{"other_family", []*btpb.Mutation{bigtableCell("family_b", "loose", "other", 2000)}},
	} {
		if _, err := data.MutateRow(ctx, &btpb.MutateRowRequest{
			TableName: table, RowKey: []byte(row.key), Mutations: row.cells,
		}); err != nil {
			t.Fatal(err)
		}
	}
}

// Mirrors the fixture described in bigtable_integer.api.yaml.
func seedBigtableInteger(t *testing.T, project string) {
	t.Helper()
	data, table := createBigtableFixture(t, project, "bigtable-integer-probe", map[string]*adminpb.ColumnFamily{"family": {}})
	if _, err := data.MutateRow(t.Context(), &btpb.MutateRowRequest{
		TableName: table, RowKey: []byte("fixture"),
		Mutations: []*btpb.Mutation{
			bigtableCell("family", "text", "42", 1000),
			bigtableCell("family", "long", "\x00\x00\x00\x00\x00\x00\x00\x2a", 1000),
			bigtableCell("family", "short", "\x00\x00\x00\x2a", 1000),
			bigtableCell("family", "empty", "", 1000),
		},
	}); err != nil {
		t.Fatal(err)
	}
}

// Mirrors the fixture described in bigtable_float_boolean.api.yaml.
func seedBigtableFloatBoolean(t *testing.T, project string) {
	t.Helper()
	data, table := createBigtableFixture(t, project, "bigtable-float-boolean-probe", map[string]*adminpb.ColumnFamily{"family": {}})
	if _, err := data.MutateRow(t.Context(), &btpb.MutateRowRequest{
		TableName: table, RowKey: []byte("fixture"),
		Mutations: []*btpb.Mutation{
			bigtableCell("family", "double", "\x3f\xf8\x00\x00\x00\x00\x00\x00", 1000),
			bigtableCell("family", "single", "\x3f\xc0\x00\x00", 1000),
			bigtableCell("family", "three", "\x00\x00\x00", 1000),
			bigtableCell("family", "true", "\xff", 1000),
			bigtableCell("family", "one", "\x01", 1000),
			bigtableCell("family", "two", "\x00\xff", 1000),
		},
	}); err != nil {
		t.Fatal(err)
	}
}

// Mirrors the fixture described in bigtable_text_encoding.api.yaml.
func seedBigtableTextEncoding(t *testing.T, project string) {
	t.Helper()
	data, table := createBigtableFixture(t, project, "bigtable-text-encoding-probe", map[string]*adminpb.ColumnFamily{"family": {}})
	var mutations []*btpb.Mutation
	for _, cell := range [][2]string{
		{"int_plain", "42"},
		{"int_negative", "-42"},
		{"int_plus", "+42"},
		{"int_leading_zero", "042"},
		{"int_leading_space", " 42"},
		{"int_trailing_space", "42 "},
		{"int_max", "9223372036854775807"},
		{"int_min", "-9223372036854775808"},
		{"int_overflow", "9223372036854775808"},
		{"int_decimal", "42.0"},
		{"int_exponent", "4e1"},
		{"int_hex", "0x2a"},
		{"int_underscore", "4_2"},
		{"int_empty", ""},
		{"int_alpha", "abc"},
		{"float_integer", "2"},
		{"float_negative", "-0.5"},
		{"float_plus", "+1.5"},
		{"float_exponent", "1e3"},
		{"float_leading_dot", ".5"},
		{"float_trailing_dot", "1."},
		{"float_leading_space", " 1.5"},
		{"float_trailing_space", "1.5 "},
		{"float_nan", "NaN"},
		{"float_nan_lower", "nan"},
		{"float_infinity", "Infinity"},
		{"float_negative_infinity", "-Infinity"},
		{"float_inf", "inf"},
		{"float_overflow", "1e400"},
		{"float_java_suffix", "1.5d"},
		{"float_comma", "1,5"},
		{"float_hex", "0x1p3"},
		{"float_empty", ""},
		{"float_alpha", "abc"},
		{"bool_false", "false"},
		{"bool_upper", "TRUE"},
		{"bool_mixed", "True"},
		{"bool_upper_false", "FALSE"},
		{"bool_one", "1"},
		{"bool_zero", "0"},
		{"bool_t", "t"},
		{"bool_yes", "yes"},
		{"bool_leading_space", " true"},
		{"bool_empty", ""},
		{"bool_alpha", "abc"},
	} {
		mutations = append(mutations, bigtableCell("family", cell[0], cell[1], 1000))
	}
	if _, err := data.MutateRow(t.Context(), &btpb.MutateRowRequest{
		TableName: table, RowKey: []byte("fixture"), Mutations: mutations,
	}); err != nil {
		t.Fatal(err)
	}
}
