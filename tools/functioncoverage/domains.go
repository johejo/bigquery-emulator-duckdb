package main

import (
	_ "embed"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"

	"github.com/johejo/bigquery-emulator-duckdb/internal/functioncoverage"
)

// domainsText lists the reviewed expansions of prose domains; its header describes the format.
//
//go:embed domains.txt
var domainsText string

var (
	signaturePattern = regexp.MustCompile(`^([A-Z][A-Z0-9_.]*)\((.*)\) -> (\S.*)$`)
	variablePattern  = regexp.MustCompile(`\$[A-Z]+`)
	listPattern      = regexp.MustCompile(`@[a-z_]+`)
)

type domainLine struct {
	number int
	text   string
}

type domainExpander struct {
	root      string
	lists     map[string][]string
	page      string
	pages     map[string]string
	functions map[string]*functioncoverage.Function
	notes     map[string]string
	// arities holds the argument counts of each function's documented syntax lines.
	arities map[string][]arity
}

func proseDomains(root string) ([]functioncoverage.Function, error) {
	return expandDomains(root, domainsText)
}

func expandDomains(root, text string) ([]functioncoverage.Function, error) {
	var lines []domainLine
	for i, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		if line != "" && !strings.HasPrefix(line, "#") {
			lines = append(lines, domainLine{i + 1, line})
		}
	}
	e := &domainExpander{
		root: root, lists: map[string][]string{}, pages: map[string]string{},
		functions: map[string]*functioncoverage.Function{}, notes: map[string]string{},
		arities: map[string][]arity{},
	}
	if err := e.run(lines, map[string]string{}); err != nil {
		return nil, err
	}
	for name, note := range e.notes {
		f := e.functions[name]
		if f == nil {
			return nil, fmt.Errorf("domains.txt: note for unenumerated function %s", name)
		}
		f.Note = note
	}
	for name, f := range e.functions {
		if err := checkArities(f, e.arities[name]); err != nil {
			return nil, fmt.Errorf("domains.txt: %w", err)
		}
	}
	// The generic domains rely on this saved specification as well as each function page.
	if _, err := os.Stat(filepath.Join(root, "reference/standard-sql/data-types.md")); err != nil {
		return nil, err
	}
	var result []functioncoverage.Function
	for _, f := range e.functions {
		result = append(result, *f)
	}
	return result, nil
}

func (e *domainExpander) run(lines []domainLine, vars map[string]string) error {
	for i := 0; i < len(lines); i++ {
		line := lines[i]
		if header, ok := strings.CutPrefix(line.text, "for "); ok {
			end := blockEnd(lines, i)
			if end < 0 {
				return fmt.Errorf("domains.txt:%d: for without end", line.number)
			}
			if err := e.loop(line.number, header, lines[i+1:end], vars); err != nil {
				return err
			}
			i = end
			continue
		}
		text, err := substitute(line.text, vars)
		if err == nil {
			err = e.statement(text)
		}
		if err != nil {
			return fmt.Errorf("domains.txt:%d: %w", line.number, err)
		}
	}
	return nil
}

func blockEnd(lines []domainLine, start int) int {
	depth := 0
	for i := start; i < len(lines); i++ {
		if strings.HasPrefix(lines[i].text, "for ") {
			depth++
		} else if lines[i].text == "end" {
			depth--
			if depth == 0 {
				return i
			}
		}
	}
	return -1
}

func (e *domainExpander) loop(number int, header string, body []domainLine, vars map[string]string) error {
	variable, list, ok := strings.Cut(header, " in ")
	if !ok || variablePattern.FindString(variable) != variable {
		return fmt.Errorf("domains.txt:%d: invalid for: %q", number, header)
	}
	if _, ok := vars[variable]; ok {
		return fmt.Errorf("domains.txt:%d: %s is already bound", number, variable)
	}
	list, err := substitute(list, vars)
	var types []string
	if err == nil {
		types, err = e.list(list)
	}
	if err != nil {
		return fmt.Errorf("domains.txt:%d: %w", number, err)
	}
	for _, t := range types {
		inner := map[string]string{variable: t}
		for k, v := range vars {
			inner[k] = v
		}
		if err := e.run(body, inner); err != nil {
			return err
		}
	}
	return nil
}

func substitute(text string, vars map[string]string) (string, error) {
	var err error
	text = variablePattern.ReplaceAllStringFunc(text, func(v string) string {
		t, ok := vars[v]
		if !ok && err == nil {
			err = fmt.Errorf("unbound %s", v)
		}
		return t
	})
	return text, err
}

// statement applies a line outside a loop's header and end.
func (e *domainExpander) statement(text string) error {
	switch {
	case text == "end":
		return fmt.Errorf("end without for")
	case strings.HasPrefix(text, "page "):
		e.page = strings.TrimPrefix(text, "page ")
		return nil
	case strings.HasPrefix(text, "types "):
		name, list, ok := strings.Cut(strings.TrimPrefix(text, "types "), " = ")
		if !ok || listPattern.FindString(name) != name {
			return fmt.Errorf("invalid types: %q", text)
		}
		if _, ok := e.lists[name]; ok {
			return fmt.Errorf("duplicate types %s", name)
		}
		types, err := e.list(list)
		e.lists[name] = types
		return err
	case strings.HasPrefix(text, "note "):
		name, note, ok := strings.Cut(strings.TrimPrefix(text, "note "), " ")
		if !ok || e.notes[name] != "" {
			return fmt.Errorf("invalid or duplicate note: %q", text)
		}
		e.notes[name] = note
		return nil
	}
	m := signaturePattern.FindStringSubmatch(text)
	if m == nil {
		return fmt.Errorf("invalid line: %q", text)
	}
	forms, err := argumentForms(m[2])
	if err != nil {
		return err
	}
	f, err := e.function(m[1])
	if err != nil {
		return err
	}
	for _, args := range forms {
		s := functioncoverage.Signature{Arguments: []string{}, Result: m[3]}
		for i, arg := range args {
			arg, literal := strings.CutPrefix(arg, "literal ")
			t, sample, _ := strings.Cut(arg, " = ")
			s.Arguments = append(s.Arguments, t)
			if sample != "" {
				if s.Samples == nil {
					s.Samples = make([]string, len(args))
				}
				s.Samples[i] = sample
			}
			if literal {
				s.LiteralArguments = append(s.LiteralArguments, i)
			}
		}
		f.Signatures = append(f.Signatures, s)
	}
	return nil
}

// function returns the entry for name, checking that the current page documents it.
func (e *domainExpander) function(name string) (*functioncoverage.Function, error) {
	source := "third_party/bigquery_docs/reference/standard-sql/" + e.page + ".md"
	if f := e.functions[name]; f != nil {
		if f.Source != source {
			return nil, fmt.Errorf("%s is already enumerated from %s", name, f.Source)
		}
		return f, nil
	}
	if e.page == "" {
		return nil, fmt.Errorf("signature without page")
	}
	text, ok := e.pages[e.page]
	if !ok {
		data, err := os.ReadFile(filepath.Join(e.root, "reference/standard-sql", e.page+".md"))
		if err != nil {
			return nil, err
		}
		text = string(data)
		e.pages[e.page] = text
	}
	section, ok := functionSection(text, name)
	if !ok {
		return nil, fmt.Errorf("%s: missing reviewed function section %s", e.page, name)
	}
	arities, err := syntaxArities(section, name)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", e.page, err)
	}
	e.arities[name] = arities
	f := &functioncoverage.Function{Name: name, Source: source}
	e.functions[name] = f
	return f, nil
}

// list expands ITEM | ITEM..., where an item can use one named list and -ITEM removes types.
func (e *domainExpander) list(text string) ([]string, error) {
	var result []string
	for _, item := range strings.Split(text, "|") {
		item = strings.TrimSpace(item)
		item, remove := strings.CutPrefix(item, "-")
		if item == "" || len(listPattern.FindAllString(item, -1)) > 1 {
			return nil, fmt.Errorf("invalid types item %q", item)
		}
		var types []string
		if name := listPattern.FindString(item); name == "" {
			types = []string{item}
		} else if list, ok := e.lists[name]; !ok {
			return nil, fmt.Errorf("unknown types %s", name)
		} else {
			for _, t := range list {
				types = append(types, strings.Replace(item, name, t, 1))
			}
		}
		if remove {
			for _, t := range types {
				if !slices.Contains(result, t) {
					return nil, fmt.Errorf("cannot remove absent type %s", t)
				}
				result = slices.DeleteFunc(result, func(u string) bool { return u == t })
			}
		} else {
			result = append(result, types...)
		}
	}
	return result, nil
}

// argumentForms expands ARG, ARG[, ARG[, ARG]] from the shortest form to the longest.
func argumentForms(text string) ([][]string, error) {
	open := -1
	if err := scan(text, func(i int, c byte) bool {
		if c == '[' {
			open = i
			return false
		}
		return true
	}); err != nil {
		return nil, err
	}
	if open < 0 {
		args, err := splitArguments(text)
		return [][]string{args}, err
	}
	if !strings.HasSuffix(text, "]") {
		return nil, fmt.Errorf("optional arguments must end the list: %q", text)
	}
	required, err := splitArguments(text[:open])
	if err != nil {
		return nil, err
	}
	inner := text[open+1 : len(text)-1]
	if len(required) > 0 {
		var ok bool
		if inner, ok = strings.CutPrefix(inner, ", "); !ok {
			return nil, fmt.Errorf("optional argument without separator: %q", text)
		}
	}
	optional, err := argumentForms(inner)
	if err != nil {
		return nil, err
	}
	result := [][]string{required}
	for _, args := range optional {
		result = append(result, append(slices.Clone(required), args...))
	}
	return result, nil
}

// splitArguments splits at commas outside type parameters and quoted samples.
func splitArguments(text string) ([]string, error) {
	args := []string{}
	start := 0
	err := scan(text, func(i int, c byte) bool {
		if c == ',' {
			args = append(args, strings.TrimSpace(text[start:i]))
			start = i + 1
		}
		return true
	})
	if last := strings.TrimSpace(text[start:]); last != "" || len(args) > 0 {
		args = append(args, last)
	}
	if slices.Contains(args, "") {
		return nil, fmt.Errorf("empty argument in %q", text)
	}
	return args, err
}

// scan calls visit with each character outside type parameters and quoted samples until it
// returns false.
func scan(text string, visit func(int, byte) bool) error {
	depth, quoted := 0, false
	for i := 0; i < len(text); i++ {
		switch c := text[i]; {
		case c == '\'':
			quoted = !quoted
		case quoted:
		case c == '<':
			depth++
		case c == '>':
			depth--
		case depth == 0 && !visit(i, c):
			return nil
		}
	}
	if depth != 0 || quoted {
		return fmt.Errorf("unbalanced type parameters or quotes: %q", text)
	}
	return nil
}

// functionSection returns the text under a function's level 2 or 3 heading up to the next
// heading of the same or a higher level.
func functionSection(text, name string) (string, bool) {
	for _, level := range []string{"## ", "### "} {
		heading := level + "`" + name + "`\n"
		_, after, ok := strings.Cut(text, "\n"+heading)
		if !ok {
			continue
		}
		lines := strings.SplitAfter(after, "\n")
		for i, line := range lines {
			if hashes, _, ok := strings.Cut(line, " "); ok && hashes != "" && strings.Trim(hashes, "#") == "" && len(hashes) <= len(level)-1 {
				lines = lines[:i]
				break
			}
		}
		return strings.Join(lines, ""), true
	}
	return "", false
}

// arity is the range of argument counts a syntax line allows; max is -1 when unbounded.
type arity struct{ min, max int }

func (a arity) allows(n int) bool { return n >= a.min && (a.max < 0 || n <= a.max) }

func (a arity) String() string {
	switch a.max {
	case -1:
		return fmt.Sprintf("%d or more", a.min)
	case a.min:
		return fmt.Sprint(a.min)
	}
	return fmt.Sprintf("%d to %d", a.min, a.max)
}

// syntaxArities reads the code blocks of a section that start with a call to name. Only the
// call's parentheses are read, so an aggregate's OVER clause is left out.
func syntaxArities(section, name string) ([]arity, error) {
	var result []arity
	blocks := strings.Split(section, "```")
	for i := 1; i < len(blocks); i += 2 {
		block := strings.TrimSpace(blocks[i])
		if !strings.HasPrefix(block, name+"(") {
			continue
		}
		args, ok := callArguments(block[len(name):])
		if !ok {
			return nil, fmt.Errorf("unbalanced syntax for %s: %q", name, block)
		}
		a, err := syntaxArity(args)
		if err != nil {
			return nil, fmt.Errorf("syntax for %s: %w", name, err)
		}
		result = append(result, a)
	}
	if len(result) == 0 {
		return nil, fmt.Errorf("no syntax line for %s", name)
	}
	return result, nil
}

// callArguments returns the text between the parenthesis that text starts with and its match.
func callArguments(text string) (string, bool) {
	depth := 0
	for i, c := range text {
		switch c {
		case '(':
			depth++
		case ')':
			if depth--; depth == 0 {
				return text[1:i], true
			}
		}
	}
	return "", false
}

// syntaxArity counts the arguments of a syntax line's argument list by its notation alone,
// without naming keywords:
//
//   - ARG[, ARG] makes the arguments after the comma optional.
//   - An argument that is only [ARG], such as a named argument, is optional.
//   - Other brackets next to an argument, such as [ DISTINCT ] or [ WHERE ... ], are clauses.
//   - ... repeats the arguments before it; an argument after it, as XN in X1,...,XN, names the
//     last repetition.
func syntaxArity(text string) (arity, error) {
	var result arity
	add := func(n int) {
		if result.max >= 0 {
			result.max += n
		}
	}
	items := syntaxItems(text)
	if len(items) == 1 && strings.TrimSpace(items[0]) == "" {
		return result, nil
	}
	for _, item := range items {
		core, groups := syntaxGroups(item)
		if core == "..." {
			result.max = -1
			return result, nil
		}
		if core == "" && len(groups) == 0 {
			return arity{}, fmt.Errorf("empty argument in %q", text)
		}
		if core != "" {
			result.min++
			add(1)
		}
		for _, g := range groups {
			rest, continues := strings.CutPrefix(g, ",")
			if !continues && core != "" {
				continue
			}
			inner, err := syntaxArity(rest)
			if err != nil {
				return arity{}, err
			}
			if inner.max < 0 {
				result.max = -1
			}
			add(inner.max)
		}
	}
	return result, nil
}

// syntaxItems splits at commas outside brackets, braces and parentheses.
func syntaxItems(text string) []string {
	var items []string
	depth, start := 0, 0
	for i, c := range text {
		switch c {
		case '[', '{', '(':
			depth++
		case ']', '}', ')':
			depth--
		case ',':
			if depth == 0 {
				items = append(items, text[start:i])
				start = i + 1
			}
		}
	}
	return append(items, text[start:])
}

// syntaxGroups separates an item's text outside brackets from the trimmed contents of its
// outermost brackets.
func syntaxGroups(item string) (string, []string) {
	var core strings.Builder
	var groups []string
	depth, start := 0, 0
	for i, c := range item {
		switch {
		case c == '[':
			if depth == 0 {
				start = i + 1
			}
			depth++
		case c == ']':
			if depth--; depth == 0 {
				groups = append(groups, strings.TrimSpace(item[start:i]))
			}
		case depth == 0:
			core.WriteRune(c)
		}
	}
	return strings.TrimSpace(core.String()), groups
}

// checkArities requires each enumerated signature to have a documented argument count, and
// each count a bounded syntax line allows to be enumerated unless a note explains the gap.
func checkArities(f *functioncoverage.Function, documented []arity) error {
	enumerated := map[int]bool{}
	for _, s := range f.Signatures {
		n := len(s.Arguments)
		enumerated[n] = true
		if !slices.ContainsFunc(documented, func(a arity) bool { return a.allows(n) }) {
			return fmt.Errorf("%s(%s) does not match the documented argument counts %v", f.Name, strings.Join(s.Arguments, ", "), documented)
		}
	}
	for _, a := range documented {
		if !enumerated[a.min] {
			return fmt.Errorf("%s does not enumerate %d arguments", f.Name, a.min)
		}
		for n := a.min + 1; a.max >= 0 && n <= a.max; n++ {
			if !enumerated[n] && f.Note == "" {
				return fmt.Errorf("%s does not enumerate %d arguments and has no note", f.Name, n)
			}
		}
	}
	return nil
}
