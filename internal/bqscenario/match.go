package bqscenario

import (
	"encoding/json"
	"fmt"
	"math/big"
	"strings"
)

// match compares selected observations. Matchers are explicit single-key objects;
// ordinary objects retain exact key comparison, including missing versus null.
func match(got any, present bool, want any, path string) error {
	if object, ok := want.(map[string]any); ok && len(object) == 1 {
		for op, value := range object {
			switch op {
			case "$schema":
				actual, err := canonicalSchema(got)
				if err != nil || !present {
					break
				}
				expected, err := canonicalSchema(value)
				if err != nil {
					return err
				}
				return match(actual, true, expected, path)
			case "$not":
				if match(got, present, value, path) != nil {
					return nil
				}
			case "$allOf":
				choices, ok := value.([]any)
				if !ok || len(choices) == 0 {
					return fmt.Errorf("%s: $allOf requires choices", path)
				}
				for _, choice := range choices {
					if err := match(got, present, choice, path); err != nil {
						return err
					}
				}
				return nil
			case "$missing":
				if value != true {
					return fmt.Errorf("%s: $missing requires true", path)
				}
				if !present {
					return nil
				}
			case "$oneOf":
				choices, ok := value.([]any)
				if !ok || len(choices) == 0 {
					return fmt.Errorf("%s: $oneOf requires choices", path)
				}
				for _, choice := range choices {
					if match(got, present, choice, path) == nil {
						return nil
					}
				}
			case "$contains":
				text, ok := got.(string)
				part, valid := value.(string)
				if present && ok && valid && strings.Contains(text, part) {
					return nil
				}
			case "$gt", "$gte", "$lt", "$lte":
				a, ok := rational(got)
				b, valid := rational(value)
				if present && ok && valid {
					cmp := a.Cmp(b)
					if op == "$gt" && cmp > 0 || op == "$gte" && cmp >= 0 || op == "$lt" && cmp < 0 || op == "$lte" && cmp <= 0 {
						return nil
					}
				}
			case "$subset":
				actual, ok := got.(map[string]any)
				expected, valid := value.(map[string]any)
				if !present || !ok || !valid {
					break
				}
				for key, want := range expected {
					value, found := actual[key]
					if err := match(value, found, want, path+"/"+key); err != nil {
						return err
					}
				}
				return nil
			default:
				if strings.HasPrefix(op, "$") {
					return fmt.Errorf("%s: unknown matcher %s", path, op)
				}
				return matchObject(got, present, object, path)
			}
			return fmt.Errorf("%s: got %s (present=%v), want %s", path, encoded(got), present, encoded(want))
		}
	}
	if object, ok := want.(map[string]any); ok {
		return matchObject(got, present, object, path)
	}
	if array, ok := want.([]any); ok {
		actual, valid := got.([]any)
		if !present || !valid || len(actual) != len(array) {
			return fmt.Errorf("%s: got %s, want %s", path, encoded(got), encoded(want))
		}
		for i, value := range array {
			if err := match(actual[i], true, value, fmt.Sprintf("%s/%d", path, i)); err != nil {
				return err
			}
		}
		return nil
	}
	if present && encoded(got) == encoded(want) {
		return nil
	}
	return fmt.Errorf("%s: got %s (present=%v), want %s", path, encoded(got), present, encoded(want))
}

func matchObject(got any, present bool, want map[string]any, path string) error {
	actual, ok := got.(map[string]any)
	if !present || !ok {
		return fmt.Errorf("%s: expected object, got %s", path, encoded(got))
	}
	for key, value := range want {
		got, found := actual[key]
		if err := match(got, found, value, path+"/"+key); err != nil {
			return err
		}
	}
	for key := range actual {
		if _, found := want[key]; !found {
			return fmt.Errorf("%s/%s: unexpected value %s", path, key, encoded(actual[key]))
		}
	}
	return nil
}

func encoded(value any) string { data, _ := json.Marshal(value); return string(data) }
func rational(value any) (*big.Rat, bool) {
	switch value := value.(type) {
	case string:
		return new(big.Rat).SetString(value)
	case json.Number:
		return new(big.Rat).SetString(string(value))
	}
	return nil, false
}
