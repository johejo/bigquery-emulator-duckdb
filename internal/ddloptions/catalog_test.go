package ddloptions

import (
	"path/filepath"
	"strings"
	"testing"
)

const catalogPath = "../../tools/ddl_options.json"

func load(t *testing.T) *Catalog {
	t.Helper()
	c, err := Load(catalogPath)
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func rejects(t *testing.T, c *Catalog, message string) {
	t.Helper()
	if err := c.Validate(); err == nil || !strings.Contains(err.Error(), message) {
		t.Errorf("Validate() = %v, want %q", err, message)
	}
}

func context(c *Catalog, group string) *Context {
	for i := range c.Contexts {
		if c.Contexts[i].Group == group {
			return &c.Contexts[i]
		}
	}
	return nil
}

func TestCheckedInCatalogIsValid(t *testing.T) {
	t.Parallel()
	if len(load(t).Cases()) == 0 {
		t.Error("no cases")
	}
}

func TestSaveRoundTrips(t *testing.T) {
	t.Parallel()
	c := load(t)
	path := filepath.Join(t.TempDir(), "catalog.json")
	if err := c.Save(path); err != nil {
		t.Fatal(err)
	}
	if _, err := Load(path); err != nil {
		t.Fatal(err)
	}
}

func TestRejectsDuplicateOptions(t *testing.T) {
	t.Parallel()
	c := load(t)
	options := c.Groups["schema_option_list"]
	c.Groups["schema_option_list"] = append(options, options[0])
	rejects(t, c, "duplicate option")
}

func TestRejectsDuplicateSamples(t *testing.T) {
	t.Parallel()
	c := load(t)
	option := &c.Groups["schema_option_list"][0]
	option.Values = append(option.Values, option.Values[0])
	rejects(t, c, "duplicate sample")
}

func TestRejectsStaleRestrictions(t *testing.T) {
	t.Parallel()
	c := load(t)
	c.Contexts[0].Only = []string{"nonexistent"}
	rejects(t, c, "unknown option in only")
}

func TestRejectsOptionsHiddenByContextRestrictions(t *testing.T) {
	t.Parallel()
	c := load(t)
	context(c, "schema_option_list").Exclude = []string{"failover_reservation"}
	rejects(t, c, "option excluded from every context: schema_option_list.failover_reservation")
}

func TestRejectsInvalidTemplates(t *testing.T) {
	t.Parallel()
	c := load(t)
	context(c, "generated_column_options").Template = "CREATE TABLE n (a INT64)"
	rejects(t, c, "template must contain {option} exactly once")
}

func TestRejectsUnreferencedGroups(t *testing.T) {
	t.Parallel()
	c := load(t)
	c.Groups["new_options"] = nil
	rejects(t, c, "no context for option group new_options")
}

func TestCasesSetOneOptionPerSample(t *testing.T) {
	t.Parallel()
	c := &Catalog{
		Source: "https://example.com/ddl",
		Groups: map[string][]Option{"g": {
			{Name: "a", Type: "STRING", Values: []string{"'x'", "NULL"}},
			{Name: "b", Type: "BOOL", Source: "elsewhere"},
		}},
		Contexts: []Context{
			{Name: "CREATE T", Group: "g", Template: "CREATE T OPTIONS ({option})"},
			{Name: "CREATE U", Group: "g", Only: []string{"a"}},
		},
	}
	cases := c.Cases()
	if len(cases) != 3 {
		t.Fatalf("Cases() = %+v", cases)
	}
	if got := strings.Join(cases[0].Queries, "; "); got != "CREATE T OPTIONS (a = 'x'); CREATE T OPTIONS (a = NULL)" {
		t.Errorf("queries = %s", got)
	}
	if cases[1].Source != "https://example.com/ddl#elsewhere" || cases[1].Queries != nil {
		t.Errorf("an option without samples = %+v", cases[1])
	}
	if cases[2].Context.Name != "CREATE U" || cases[2].Queries != nil {
		t.Errorf("a context without a template = %+v", cases[2])
	}
}
