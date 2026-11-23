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
