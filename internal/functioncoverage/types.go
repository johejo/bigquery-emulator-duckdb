package functioncoverage

import (
	"fmt"
	"strings"
)

// SplitTypes splits a comma-separated type list without splitting nested ARRAY/STRUCT types.
func SplitTypes(text string) ([]string, error) {
	if text == "" {
		return nil, nil
	}
	depth, start := 0, 0
	var parts []string
	for i, r := range text {
		switch r {
		case '<':
			depth++
		case '>':
			depth--
			if depth < 0 {
				return nil, fmt.Errorf("unbalanced type %q", text)
			}
		case ',':
			if depth == 0 {
				parts = append(parts, strings.TrimSpace(text[start:i]))
				start = i + 1
			}
		}
	}
	if depth != 0 {
		return nil, fmt.Errorf("unbalanced type %q", text)
	}
	parts = append(parts, strings.TrimSpace(text[start:]))
	for _, p := range parts {
		if p == "" {
			return nil, fmt.Errorf("empty type in %q", text)
		}
	}
	return parts, nil
}

// Field is the REST schema metadata needed to check scalar and nested result types. Names are
// included inside STRUCT results; top-level result aliases are not part of the function's type.
type Field struct {
	Name             string  `json:"name"`
	Type             string  `json:"type"`
	Mode             string  `json:"mode"`
	Fields           []Field `json:"fields"`
	RangeElementType *struct {
		Type string `json:"type"`
	} `json:"rangeElementType"`
}

func (f Field) SQLType() (string, error) {
	name := f.Type
	switch name {
	case "INTEGER":
		name = "INT64"
	case "FLOAT":
		name = "FLOAT64"
	case "BOOLEAN":
		name = "BOOL"
	case "RECORD", "STRUCT":
		var fields []string
		for _, child := range f.Fields {
			t, err := child.SQLType()
			if err != nil {
				return "", err
			}
			if child.Name == "" {
				return "", fmt.Errorf("record field has no name")
			}
			fields = append(fields, child.Name+" "+t)
		}
		name = "STRUCT<" + strings.Join(fields, ", ") + ">"
	case "RANGE":
		if f.RangeElementType == nil {
			return "", fmt.Errorf("range schema has no element type")
		}
		switch f.RangeElementType.Type {
		case "DATE", "DATETIME", "TIMESTAMP":
		default:
			return "", fmt.Errorf("invalid range element type %q", f.RangeElementType.Type)
		}
		name = "RANGE<" + f.RangeElementType.Type + ">"
	}
	if name == "" {
		return "", fmt.Errorf("schema field has no type")
	}
	if f.Mode == "REPEATED" {
		name = "ARRAY<" + name + ">"
	}
	return name, nil
}

func containerSample(t string) (string, bool) {
	if strings.HasPrefix(t, "ARRAY<") && strings.HasSuffix(t, ">") {
		element := t[len("ARRAY<") : len(t)-1]
		// BigQuery forbids arrays of arrays; nested arrays are represented through STRUCT fields.
		if strings.HasPrefix(element, "ARRAY<") {
			return "", false
		}
		sample, ok := Sample(element)
		if !ok {
			return "", false
		}
		return t + "[" + sample + "]", true
	}
	if strings.HasPrefix(t, "STRUCT<") && strings.HasSuffix(t, ">") {
		fields, err := SplitTypes(t[len("STRUCT<") : len(t)-1])
		if err != nil {
			return "", false
		}
		var samples []string
		for _, field := range fields {
			_, typ, ok := strings.Cut(field, " ")
			if !ok {
				return "", false
			}
			sample, ok := Sample(typ)
			if !ok {
				return "", false
			}
			samples = append(samples, sample)
		}
		return t + "(" + strings.Join(samples, ", ") + ")", true
	}
	if strings.HasPrefix(t, "RANGE<") && strings.HasSuffix(t, ">") {
		switch t {
		case "RANGE<DATE>":
			return t + " '[2024-01-01, 2024-02-01)'", true
		case "RANGE<DATETIME>":
			return t + " '[2024-01-01 00:00:00, 2024-02-01 00:00:00)'", true
		case "RANGE<TIMESTAMP>":
			return t + " '[2024-01-01 00:00:00+00, 2024-02-01 00:00:00+00)'", true
		}
	}
	return "", false
}
