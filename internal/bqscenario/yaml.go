package bqscenario

import (
	"encoding/json"
	"fmt"
	"strings"

	"github.com/goccy/go-yaml"
	"github.com/goccy/go-yaml/ast"
	"github.com/goccy/go-yaml/parser"
)

// IsFile reports whether path names a scenario, excluding answer sidecars.
func IsFile(path string) bool {
	return strings.HasSuffix(path, ".api.json") || strings.HasSuffix(path, ".api.yaml") || strings.HasSuffix(path, ".api.yml")
}

func isYAML(path string) bool {
	return strings.HasSuffix(path, ".yaml") || strings.HasSuffix(path, ".yml")
}

// Convert the JSON data model using YAML's syntax. Read numeric tokens directly so
// neither large integers nor decimal literals pass through float64. YAML-specific
// types, non-string keys and aliases are rejected; templates provide explicit reuse.
func yamlJSON(data []byte) ([]byte, error) {
	file, err := parser.ParseBytes(data, 0)
	if err != nil {
		return nil, err
	}
	if len(file.Docs) != 1 {
		return nil, fmt.Errorf("expected one YAML document")
	}
	value, err := yamlValue(file.Docs[0].Body)
	if err != nil {
		return nil, err
	}
	return json.Marshal(value)
}

func yamlValue(node ast.Node) (any, error) {
	switch node := node.(type) {
	case *ast.MappingNode:
		values := map[string]any{}
		for _, entry := range node.Values {
			key, ok := entry.Key.(*ast.StringNode)
			if !ok {
				return nil, fmt.Errorf("YAML object keys must be strings")
			}
			if _, exists := values[key.Value]; exists {
				return nil, fmt.Errorf("duplicate key %q", key.Value)
			}
			value, err := yamlValue(entry.Value)
			if err != nil {
				return nil, err
			}
			values[key.Value] = value
		}
		return values, nil
	case *ast.SequenceNode:
		values := make([]any, len(node.Values))
		for i, entry := range node.Values {
			value, err := yamlValue(entry)
			if err != nil {
				return nil, err
			}
			values[i] = value
		}
		return values, nil
	case *ast.IntegerNode, *ast.FloatNode:
		number := node.GetToken().Value
		if !json.Valid([]byte(number)) {
			return nil, fmt.Errorf("use a JSON number or quote %q", number)
		}
		return json.Number(number), nil
	case *ast.StringNode:
		return node.Value, nil
	case *ast.LiteralNode:
		var value string
		if err := yaml.NodeToValue(node, &value); err != nil {
			return nil, err
		}
		return value, nil
	case *ast.BoolNode:
		return node.Value, nil
	case *ast.NullNode:
		return nil, nil
	default:
		return nil, fmt.Errorf("unsupported YAML node %T; use JSON-compatible values and scenario templates", node)
	}
}
