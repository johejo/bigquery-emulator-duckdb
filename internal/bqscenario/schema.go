package bqscenario

import (
	"encoding/json"
	"fmt"
)

// canonicalSchema compares the TableFieldSchema fields exposed by the Go SDK.
// Omitted/null optional fields take their documented zero/default values; unknown
// resource fields remain outside this comparison, as in the former SDK assertions.
func canonicalSchema(value any) ([]any, error) {
	fields, ok := value.([]any)
	if !ok {
		return nil, fmt.Errorf("schema must be a field array")
	}
	result := make([]any, len(fields))
	for i, value := range fields {
		field, ok := value.(map[string]any)
		if !ok {
			return nil, fmt.Errorf("schema field %d must be an object", i)
		}
		normalized := map[string]any{
			"name": "", "type": "", "mode": "NULLABLE", "description": "", "precision": "0", "scale": "0", "maxLength": "0", "defaultValueExpression": "", "roundingMode": "", "collation": "", "timestampPrecision": "0", "policyTags": nil, "rangeElementType": nil,
		}
		for key, defaultValue := range normalized {
			if value, found := field[key]; found && value != nil {
				if number, ok := value.(json.Number); ok {
					value = string(number)
				}
				if key == "mode" && value == "" {
					value = defaultValue
				}
				normalized[key] = value
			}
		}
		var children []any
		if value, found := field["fields"]; found && value != nil {
			var err error
			children, err = canonicalSchema(value)
			if err != nil {
				return nil, err
			}
		} else {
			children = []any{}
		}
		normalized["fields"] = children
		result[i] = normalized
	}
	return result, nil
}
