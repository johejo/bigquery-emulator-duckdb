package main

import (
	"encoding/json"
	"fmt"
	"strings"
)

type sqlCase struct {
	line       int
	sql        string
	parameters []json.RawMessage
	answered   bool
}

// parseSQLCases checks the case envelope, not SQL syntax. In particular, a final query must
// have an answer marker even when its answer has not been collected yet.
func parseSQLCases(path string, lines []string) ([]sqlCase, error) {
	var cases []sqlCase
	var sql []string
	var parameters []json.RawMessage
	start := 0
	fail := func(line int, message string) error {
		return fmt.Errorf("%s:%d: %s", path, line, message)
	}
	for i, line := range lines {
		text := strings.TrimSpace(line)
		switch {
		case text == "" || strings.HasPrefix(text, "#"):
			if start != 0 && len(parameters) == 0 {
				sql = append(sql, line)
			}
		case strings.HasPrefix(text, "?>"):
			if start == 0 {
				return nil, fail(i+1, "answer marker has no query")
			}
			if line != "?>" && !strings.HasPrefix(line, "?> ") {
				return nil, fail(i+1, "expected ?> or ?> followed by a space and an answer")
			}
			answer := strings.TrimPrefix(line, "?>")
			if answer != "" {
				answer = strings.TrimSpace(answer)
				if answer == "error:" || strings.HasPrefix(answer, "error: ") {
					if strings.TrimSpace(strings.TrimPrefix(answer, "error:")) == "" {
						return nil, fail(i+1, "error answer must have a message")
					}
				} else if !json.Valid([]byte(answer)) {
					return nil, fail(i+1, "answer must be JSON or error: followed by a message")
				}
			}
			cases = append(cases, sqlCase{line: i, sql: strings.Join(sql, "\n"), parameters: parameters, answered: answer != ""})
			sql, parameters, start = nil, nil, 0
		case strings.HasPrefix(text, "param>"):
			if start == 0 {
				return nil, fail(i+1, "parameter has no query")
			}
			if !strings.HasPrefix(line, "param> ") {
				return nil, fail(i+1, "expected param> followed by a space and a JSON object")
			}
			parameter := json.RawMessage(strings.TrimPrefix(line, "param> "))
			var object map[string]json.RawMessage
			if err := json.Unmarshal(parameter, &object); err != nil || object == nil {
				return nil, fail(i+1, "parameter must be a JSON object")
			}
			parameters = append(parameters, parameter)
		default:
			if len(parameters) > 0 {
				return nil, fail(i+1, "expected answer marker after parameters")
			}
			if start == 0 {
				start = i + 1
			}
			sql = append(sql, line)
		}
	}
	if start != 0 {
		return nil, fail(start, "query is missing its ?> answer marker")
	}
	return cases, nil
}
