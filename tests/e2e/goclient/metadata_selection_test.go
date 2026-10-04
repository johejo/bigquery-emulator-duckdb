package goclient

import (
	"context"
	"errors"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/googleapi"
)

func TestDatasetLabelFilter(t *testing.T) {
	ctx := context.Background()
	client := newClientForProject(t, "go-dataset-filter")
	for id, labels := range map[string]map[string]string{
		"a":       {"env": "prod", "team": ""},
		"b":       {"env": "dev"},
		"c":       {"env": "prod"},
		"d":       nil,
		"_hidden": {"env": "prod"},
	} {
		dataset := client.Dataset(id)
		_ = dataset.DeleteWithContents(ctx)
		if err := dataset.Create(ctx, &bigquery.DatasetMetadata{Labels: labels}); err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	}
	for _, tc := range []struct {
		filter string
		hidden bool
		want   []string
	}{
		{"labels.env:prod", false, []string{"a", "c"}},
		{"labels.env", false, []string{"a", "b", "c"}},
		{"labels.env:*", false, []string{"a", "b", "c"}},
		{"labels.env:prod labels.team", false, []string{"a"}},
		{"labels.team:*", false, []string{"a"}},
		{"labels.team:", false, []string{"a"}},
		{"labels.env:Prod", false, nil},
		{"labels.Env:prod", false, nil},
		{"labels.missing", false, nil},
		{"labels.env:prod", true, []string{"_hidden", "a", "c"}},
	} {
		t.Run(tc.filter, func(t *testing.T) {
			it := client.Datasets(ctx)
			it.Filter, it.ListHidden = tc.filter, tc.hidden
			// One entry per page checks that filtering precedes page slicing.
			if got := listDatasetIDs(t, it, nil); !reflect.DeepEqual(got, tc.want) {
				t.Fatalf("got %v, want %v", got, tc.want)
			}
		})
	}
	for _, filter := range []string{
		"env:prod", "labels.", "labels.env:prod labels.env:dev",
		"labels.env:prod:dev", "labels.a labels.b labels.c labels.d labels.e labels.f labels.g labels.h labels.i labels.j labels.k",
	} {
		it := client.Datasets(ctx)
		it.Filter = filter
		var apiError *googleapi.Error
		if _, err := it.Next(); !errors.As(err, &apiError) || apiError.Code != 400 {
			t.Errorf("filter %q: got %v, want HTTP 400", filter, err)
		}
	}
	// Ten unique expressions are valid, even if none of the datasets match.
	it := client.Datasets(ctx)
	it.Filter = "labels.a labels.b labels.c labels.d labels.e labels.f labels.g labels.h labels.i labels.j"
	if got := listDatasetIDs(t, it, nil); len(got) != 0 {
		t.Fatalf("ten expressions: got %v", got)
	}
}

func TestTableMetadataView(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_metadata_view")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	table := dataset.Table("t")
	schema := bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}}
	if err := table.Create(ctx, &bigquery.TableMetadata{Schema: schema, Description: "metadata"}); err != nil {
		t.Fatal(err)
	}
	if err := table.Inserter().Put(ctx, &bigquery.ValuesSaver{Schema: schema, Row: []bigquery.Value{int64(1)}}); err != nil {
		t.Fatal(err)
	}
	for _, view := range []bigquery.TableMetadataView{
		bigquery.BasicMetadataView, bigquery.StorageStatsMetadataView, bigquery.FullMetadataView,
		"TABLE_METADATA_VIEW_UNSPECIFIED", "",
	} {
		md, err := table.Metadata(ctx, bigquery.WithMetadataView(view))
		if err != nil {
			t.Fatal(err)
		}
		wantRows := uint64(1)
		if view == bigquery.BasicMetadataView {
			wantRows = 0
		}
		if md.NumRows != wantRows || md.Description != "metadata" || !reflect.DeepEqual(md.Schema, schema) {
			t.Errorf("view %q: got %+v", view, md)
		}
	}
	if _, err := table.Metadata(ctx, bigquery.WithMetadataView("invalid")); err == nil || !strings.Contains(err.Error(), "view") {
		t.Errorf("invalid view: got %v", err)
	}
}
