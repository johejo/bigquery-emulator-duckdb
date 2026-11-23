// Package ddloptions reads tools/ddl_options.json, a snapshot of the option lists in BigQuery's DDL
// reference, and builds the statements tools/restprobe runs to generate docs/options.md.
// tests/ddl_options_catalog_test.cc checks its types and samples with GoogleSQL and against the
// options the DDL handlers implement.
package ddloptions

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"slices"
	"strings"
)

// Option is one row of an option list: its name, its GoogleSQL type and SQL sample values.
// Source is the anchor of the reference section that describes it, when that is not its group.
// ValueLabels gives selected SQL sample values short descriptions for the support report.
type Option struct {
	Name        string            `json:"name"`
	Type        string            `json:"type"`
	Values      []string          `json:"values,omitempty"`
	ValueLabels map[string]string `json:"value_labels,omitempty"`
	Source      string            `json:"source,omitempty"`
	Operators   []string          `json:"operators,omitempty"`
}

// Context is a statement that takes the options of Group. Template holds {option} where a case
// puts one assignment, or {name} and {assignment} to qualify the name separately.
// A context without a template is listed but not probed. Templates replaces it for
// the options that need another statement, such as one the template already sets, so they stay
// in the same context. Implementation names the DDL handlers' context, which the C++ test
// compares types with. Heading optionally groups adjacent contexts in the report without
// combining their option types or probe templates.
type Context struct {
	Name           string            `json:"name"`
	Heading        string            `json:"heading,omitempty"`
	Group          string            `json:"group"`
	Template       string            `json:"template,omitempty"`
	Templates      map[string]string `json:"templates,omitempty"`
	Implementation string            `json:"implementation,omitempty"`
	Setup          []string          `json:"setup,omitempty"`
	Only           []string          `json:"only,omitempty"`
	Exclude        []string          `json:"exclude,omitempty"`
}

// Catalog maps the reference's section anchors to their options, and lists the contexts that
// take them.
type Catalog struct {
	Source   string              `json:"source"`
	Groups   map[string][]Option `json:"groups"`
	Contexts []Context           `json:"contexts"`
}

// Load reads and validates the catalog at path.
func Load(path string) (*Catalog, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var c Catalog
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&c); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	if err := c.Validate(); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return &c, nil
}

// Save writes the catalog to path in the format Load reads, replacing the file at once.
func (c *Catalog) Save(path string) error {
	var b bytes.Buffer
	encoder := json.NewEncoder(&b)
	encoder.SetEscapeHTML(false)
	encoder.SetIndent("", "  ")
	if err := encoder.Encode(c); err != nil {
		return err
	}
	temporary := path + ".tmp"
	if err := os.WriteFile(temporary, b.Bytes(), 0o644); err != nil {
		return err
	}
	return os.Rename(temporary, path)
}

// Applies reports whether the context takes the option, which its Only and Exclude decide.
func (c Context) Applies(name string) bool {
	return (c.Only == nil || slices.Contains(c.Only, name)) && !slices.Contains(c.Exclude, name)
}

func validTemplate(template string) bool {
	option, name, assignment := strings.Count(template, "{option}"), strings.Count(template, "{name}"), strings.Count(template, "{assignment}")
	return (option == 1 && name == 0 && assignment == 0) || (option == 0 && name == 1 && assignment == 1)
}

// Validate checks what keeps the catalog consistent, before any probe runs: names and samples
// are unique, templates have assignment placeholders, restrictions name options of their group, and every
// option appears in some context.
func (c *Catalog) Validate() error {
	for group, options := range c.Groups {
		names := map[string]bool{}
		for _, option := range options {
			if names[option.Name] {
				return fmt.Errorf("%s: duplicate option %s", group, option.Name)
			}
			names[option.Name] = true
			operators := map[string]bool{}
			for _, op := range option.Operators {
				if (op != "=" && op != "+=" && op != "-=") || operators[op] {
					return fmt.Errorf("%s.%s: invalid or duplicate operator %s", group, option.Name, op)
				}
				operators[op] = true
			}
			values := map[string]bool{}
			for _, value := range option.Values {
				if values[value] {
					return fmt.Errorf("%s.%s: duplicate sample %s", group, option.Name, value)
				}
				values[value] = true
			}
			for value, label := range option.ValueLabels {
				if !values[value] || strings.TrimSpace(label) == "" || strings.ContainsAny(label, "\r\n") {
					return fmt.Errorf("%s.%s: invalid label for sample %s", group, option.Name, value)
				}
			}
		}
	}
	contexts := map[string]bool{}
	listed := map[string]bool{}
	for _, context := range c.Contexts {
		if context.Name == "" {
			return fmt.Errorf("empty context name")
		}
		if contexts[context.Name] {
			return fmt.Errorf("duplicate context %s", context.Name)
		}
		contexts[context.Name] = true
		options, ok := c.Groups[context.Group]
		if !ok {
			return fmt.Errorf("%s: unknown group %s", context.Name, context.Group)
		}
		if context.Template != "" && !validTemplate(context.Template) {
			return fmt.Errorf("%s: template needs {option} or both {name} and {assignment}, exactly once", context.Name)
		}
		if len(context.Templates) > 0 && context.Template == "" {
			return fmt.Errorf("%s: templates without a template", context.Name)
		}
		for name, template := range context.Templates {
			if !context.Applies(name) || !slices.ContainsFunc(options, func(o Option) bool { return o.Name == name }) {
				return fmt.Errorf("%s: template for an option the context does not take: %s", context.Name, name)
			}
			if !validTemplate(template) {
				return fmt.Errorf("%s.%s: template needs {option} or both {name} and {assignment}, exactly once", context.Name, name)
			}
		}
		for key, names := range map[string][]string{"only": context.Only, "exclude": context.Exclude} {
			for _, name := range names {
				if !slices.ContainsFunc(options, func(o Option) bool { return o.Name == name }) {
					return fmt.Errorf("%s: unknown option in %s: %s", context.Name, key, name)
				}
			}
		}
		for _, option := range options {
			if context.Applies(option.Name) {
				listed[context.Group+"."+option.Name] = true
			}
		}
	}
	for group, options := range c.Groups {
		if !slices.ContainsFunc(c.Contexts, func(c Context) bool { return c.Group == group }) {
			return fmt.Errorf("no context for option group %s", group)
		}
		for _, option := range options {
			if !listed[group+"."+option.Name] {
				return fmt.Errorf("option excluded from every context: %s.%s", group, option.Name)
			}
		}
	}
	return nil
}

// Case is one option of a context and the statements that set it, one sample each.
type Case struct {
	Context *Context
	Option  Option
	Source  string
	Queries []Query
}

// Query pairs a probe statement with the label shown if it is rejected as unsupported.
// When no label is set, the report shows the full statement.
type Query struct {
	SQL   string
	Label string
}

// Cases lists the options each context takes, in the catalog's order. A case has no queries when
// its context has no template or its option no samples.
func (c *Catalog) Cases() []Case {
	var cases []Case
	for i := range c.Contexts {
		context := &c.Contexts[i]
		for _, option := range c.Groups[context.Group] {
			if !context.Applies(option.Name) {
				continue
			}
			source := option.Source
			if source == "" {
				source = context.Group
			}
			k := Case{Context: context, Option: option, Source: c.Source + "#" + source}
			if context.Template != "" {
				template := context.Template
				if t, ok := context.Templates[option.Name]; ok {
					template = t
				}
				operators := option.Operators
				if len(operators) == 0 {
					operators = []string{"="}
				}
				for _, op := range operators {
					for _, value := range option.Values {
						assignment := op + " " + value
						sql := strings.NewReplacer("{option}", option.Name+" "+assignment,
							"{name}", option.Name, "{assignment}", assignment).Replace(template)
						k.Queries = append(k.Queries, Query{
							SQL:   sql,
							Label: option.ValueLabels[value],
						})
					}
				}
			}
			cases = append(cases, k)
		}
	}
	return cases
}
