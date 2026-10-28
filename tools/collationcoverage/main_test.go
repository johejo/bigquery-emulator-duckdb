package main

import (
	"reflect"
	"testing"
)

func TestExtract(t *testing.T) {
	text := "## `COUNT`\n\nThis function with `DISTINCT` supports specifying [collation](x) .\n\n" +
		"## `LENGTH`\n\nReturns the length.\n\n" +
		"### Comparison operators\n\n| Equal | `X = Y` | This operator supports specifying [collation](x) . |\n" +
		"| Less Than | `X < Y` | This operator supports specifying [collation](x) . |\n\n" +
		"### `IN` operator\n\nThis operator supports [collation](x) , but these limitations apply:\n"
	got := extract("page.md", text)
	want := []Operation{
		{Name: "COUNT", Source: "page.md", Distinct: true},
		{Name: "Comparison operators", Source: "page.md"},
		{Name: "IN operator", Source: "page.md"},
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("extracted %#v", got)
	}
}
