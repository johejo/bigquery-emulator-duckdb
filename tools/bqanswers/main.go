// Command bqanswers runs on BigQuery each query of a cases file whose "?>" line is still empty,
// and writes BigQuery's answer after it: the JSON of the cell's "v" as jobs.query returns it, or
// "error: " and the message. It authenticates as `gcloud auth print-access-token` does. Only a
// maintainer runs it, since queries on BigQuery are billed.
//
// A directory processes each of its *.txt files.
//
//	bqanswers PROJECT FILE_OR_DIRECTORY
package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

type queryResponse struct {
	JobComplete bool `json:"jobComplete"`
	Rows        []struct {
		F []struct {
			V json.RawMessage `json:"v"`
		} `json:"f"`
	} `json:"rows"`
	Error *struct {
		Message string `json:"message"`
	} `json:"error"`
}

// answer runs sql on BigQuery and returns what follows "?> ", or an error when BigQuery does not
// return exactly one cell.
func answer(token, project, sql string) (string, error) {
	body, err := json.Marshal(map[string]any{"query": sql, "useLegacySql": false, "timeoutMs": 60000})
	if err != nil {
		return "", err
	}
	url := fmt.Sprintf("https://bigquery.googleapis.com/bigquery/v2/projects/%s/queries", project)
	request, err := http.NewRequest(http.MethodPost, url, bytes.NewReader(body))
	if err != nil {
		return "", err
	}
	request.Header.Set("Authorization", "Bearer "+token)
	request.Header.Set("Content-Type", "application/json")
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		return "", err
	}
	defer response.Body.Close()
	var decoded queryResponse
	if err := json.NewDecoder(response.Body).Decode(&decoded); err != nil {
		return "", err
	}
	switch {
	case decoded.Error != nil:
		return "error: " + decoded.Error.Message, nil
	case !decoded.JobComplete:
		return "", fmt.Errorf("the query did not finish within a minute")
	case len(decoded.Rows) != 1 || len(decoded.Rows[0].F) != 1:
		return "", fmt.Errorf("got %d rows, want one cell", len(decoded.Rows))
	}
	return string(decoded.Rows[0].F[0].V), nil
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: bqanswers PROJECT FILE_OR_DIRECTORY")
		os.Exit(2)
	}
	project, path := os.Args[1], os.Args[2]
	token, err := exec.Command("gcloud", "auth", "print-access-token").Output()
	if err != nil {
		fmt.Fprintf(os.Stderr, "gcloud auth print-access-token: %v\n", err)
		os.Exit(1)
	}
	if err := fillAnswers(path, func(sql string) (string, error) {
		return answer(strings.TrimSpace(string(token)), project, sql)
	}); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func fillAnswers(path string, query func(string) (string, error)) error {
	info, err := os.Stat(path)
	if err != nil {
		return err
	}
	paths := []string{path}
	if info.IsDir() {
		paths, err = filepath.Glob(filepath.Join(path, "*.txt"))
		if err != nil {
			return err
		}
	}
	var failures []error
	for _, file := range paths {
		if err := fillFile(file, query); err != nil {
			failures = append(failures, err)
		}
	}
	return errors.Join(failures...)
}

func fillFile(path string, query func(string) (string, error)) error {
	content, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	lines := strings.Split(string(content), "\n")
	var failures []error
	var sql []string
	for i, text := range lines {
		switch {
		case text == "?>":
			got, err := query(strings.Join(sql, "\n"))
			if err != nil {
				failures = append(failures, fmt.Errorf("%s:%d: %w", path, i+1, err))
			} else {
				lines[i] = "?> " + got
			}
			sql = nil
		case strings.HasPrefix(text, "?> "):
			sql = nil
		case text == "" || strings.HasPrefix(text, "#"):
			if len(sql) > 0 {
				sql = append(sql, text)
			}
		default:
			sql = append(sql, text)
		}
	}
	if err := os.WriteFile(path, []byte(strings.Join(lines, "\n")), 0o644); err != nil {
		failures = append(failures, err)
	}
	return errors.Join(failures...)
}
