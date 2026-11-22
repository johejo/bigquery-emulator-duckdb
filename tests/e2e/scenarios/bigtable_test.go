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

// Mirrors the Bigtable fixture described in bigtable_qualifiers.api.yaml.
func seedBigtableQualifiers(t *testing.T, project string) {
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
	const tableID = "bigtable-qualifier-probe"
	table := parent + "/tables/" + tableID
	if _, err := admin.CreateTable(ctx, &adminpb.CreateTableRequest{
		Parent: parent, TableId: tableID,
		Table: &adminpb.Table{ColumnFamilies: map[string]*adminpb.ColumnFamily{"family_a": {}, "family_b": {}}},
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
	cell := func(family, qualifier, value string, micros int64) *btpb.Mutation {
		return &btpb.Mutation{Mutation: &btpb.Mutation_SetCell_{SetCell: &btpb.Mutation_SetCell{
			FamilyName: family, ColumnQualifier: []byte(qualifier), Value: []byte(value), TimestampMicros: micros,
		}}}
	}
	data := btpb.NewBigtableClient(conn)
	for _, row := range []struct {
		key   string
		cells []*btpb.Mutation
	}{
		{"both", []*btpb.Mutation{
			cell("family_a", "promoted", "old", 1000), cell("family_a", "promoted", "new", 2000),
			cell("family_a", "loose", "unlisted", 2000),
			cell("family_a", "\xff", "raw-old", 1000), cell("family_a", "\xff", "raw-new", 2000),
		}},
		{"fallback_only", []*btpb.Mutation{cell("family_a", "loose", "unlisted", 2000)}},
		{"named_only", []*btpb.Mutation{cell("family_a", "promoted", "solo", 2000)}},
		{"other_family", []*btpb.Mutation{cell("family_b", "loose", "other", 2000)}},
	} {
		if _, err := data.MutateRow(ctx, &btpb.MutateRowRequest{
			TableName: table, RowKey: []byte(row.key), Mutations: row.cells,
		}); err != nil {
			t.Fatal(err)
		}
	}
}
