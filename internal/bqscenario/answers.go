package bqscenario

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

type answerFile struct {
	Version int                        `json:"version"`
	Cases   map[string]json.RawMessage `json:"cases"`
}

// AnswersPath names the machine-written observations beside a YAML scenario.
func AnswersPath(path string) string {
	return strings.TrimSuffix(path, filepath.Ext(path)) + ".answers.json"
}

func loadAnswers(path string, file *File) error {
	data, err := os.ReadFile(AnswersPath(path))
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	var answers answerFile
	if err := decodeJSON(data, &answers); err != nil {
		return fmt.Errorf("%s: %w", AnswersPath(path), err)
	}
	if answers.Version != 1 {
		return fmt.Errorf("unsupported answers version %d", answers.Version)
	}
	names := map[string]bool{}
	for i := range file.Cases {
		c := &file.Cases[i]
		names[c.Name] = true
		c.Answer = answers.Cases[c.Name]
		if answered(c.Answer) {
			var observation Observation
			if err := decodeJSON(c.Answer, &observation); err != nil {
				return fmt.Errorf("%s/%s: %w", AnswersPath(path), c.Name, err)
			}
			if observation.Project == "" || observation.Run == "" || observation.Steps == nil {
				return fmt.Errorf("%s/%s: incomplete observation", AnswersPath(path), c.Name)
			}
		}
	}
	for name := range answers.Cases {
		if !names[name] {
			return fmt.Errorf("%s: answer for unknown case %q", AnswersPath(path), name)
		}
	}
	return nil
}

func saveAnswers(path string, file *File) error {
	if !isYAML(path) {
		return writeScenarios(path, file)
	}
	answers := answerFile{Version: 1, Cases: map[string]json.RawMessage{}}
	for _, c := range file.Cases {
		if answered(c.Answer) {
			answers.Cases[c.Name] = c.Answer
		}
	}
	return writeJSON(AnswersPath(path), answers)
}

// ValidateCollection checks the scenario and any YAML observation sidecar before
// authentication or requests. Verification deliberately reads only the scenario.
func ValidateCollection(path string) error {
	file, err := Read(path)
	if err != nil {
		return err
	}
	if isYAML(path) {
		return loadAnswers(path, file)
	}
	return nil
}
