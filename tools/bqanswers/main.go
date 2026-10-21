// Command bqanswers runs on BigQuery each query of a cases file whose "?>" line is still empty,
// and writes BigQuery's answer after it: the JSON of the cell's "v" as jobs.query returns it, or
// "error: " and the message. The JSON is written on one line, as cases in scalars/ are. It
// authenticates as `gcloud auth print-access-token` does. Only a maintainer runs it, since
// queries on BigQuery are billed.
//
// Lines starting with "param> " between a query and its "?>" line each give a named query
// parameter, as JSON of an entry of the request's queryParameters.
//
// A directory processes each *.txt and *.api.json file. API scenarios can use REST, multipart
// uploads and BigQuery Storage gRPC; see README.md in this directory. -validate checks scenarios
// without authenticating or sending requests.
//
//	bqanswers [-validate] [-timeout 2m] PROJECT FILE_OR_DIRECTORY
package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"slices"
	"strings"
	"time"
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
func answer(ctx context.Context, token, project, sql string, parameters []json.RawMessage) (string, error) {
	request := map[string]any{"query": sql, "useLegacySql": false, "timeoutMs": 60000}
	if len(parameters) > 0 {
		request["parameterMode"] = "NAMED"
		request["queryParameters"] = parameters
	}
	body, err := json.Marshal(request)
	if err != nil {
		return "", err
	}
	url := fmt.Sprintf("https://bigquery.googleapis.com/bigquery/v2/projects/%s/queries", project)
	post, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader(body))
	if err != nil {
		return "", err
	}
	post.Header.Set("Authorization", "Bearer "+token)
	post.Header.Set("Content-Type", "application/json")
	response, err := http.DefaultClient.Do(post)
	if err != nil {
		return "", err
	}
	defer response.Body.Close()
	return parseAnswer(response.Body)
}

// parseAnswer returns what follows "?> " for a jobs.query response.
func parseAnswer(body io.Reader) (string, error) {
	var decoded queryResponse
	if err := json.NewDecoder(body).Decode(&decoded); err != nil {
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
	var cell bytes.Buffer
	if err := json.Compact(&cell, decoded.Rows[0].F[0].V); err != nil {
		return "", err
	}
	return cell.String(), nil
}

func main() {
	validate := flag.Bool("validate", false, "validate API scenarios without authentication or requests")
	timeout := flag.Duration("timeout", 2*time.Minute, "deadline for each scenario step, including polling")
	flag.Parse()
	if flag.NArg() != 2 || *timeout <= 0 {
		fmt.Fprintln(os.Stderr, "usage: bqanswers [-validate] [-timeout 2m] PROJECT FILE_OR_DIRECTORY")
		os.Exit(2)
	}
	if err := run(flag.Arg(0), flag.Arg(1), *validate, *timeout); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func caseFiles(path string) ([]string, error) {
	info, err := os.Stat(path)
	if err != nil {
		return nil, err
	}
	if !info.IsDir() {
		if !strings.HasSuffix(path, ".txt") && !strings.HasSuffix(path, ".api.json") {
			return nil, fmt.Errorf("unsupported case file %q", path)
		}
		return []string{path}, nil
	}
	var paths []string
	for _, pattern := range []string{"*.txt", "*.api.json"} {
		matches, err := filepath.Glob(filepath.Join(path, pattern))
		if err != nil {
			return nil, err
		}
		paths = append(paths, matches...)
	}
	slices.Sort(paths)
	return paths, nil
}

func run(project, path string, validate bool, timeout time.Duration) error {
	paths, err := caseFiles(path)
	if err != nil {
		return err
	}
	// Validate every scenario before any mutations, including files that already have answers.
	for _, file := range paths {
		if strings.HasSuffix(file, ".api.json") {
			if _, err := readScenarios(file); err != nil {
				return err
			}
		}
	}
	if validate {
		return nil
	}
	token, err := exec.Command("gcloud", "auth", "print-access-token").Output()
	if err != nil {
		return fmt.Errorf("gcloud auth print-access-token: %w", err)
	}
	accessToken := strings.TrimSpace(string(token))
	transport := newScenarioTransport(accessToken)
	defer transport.Close()
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()
	return fillAnswers(ctx, path, func(sql string, parameters []json.RawMessage) (string, error) {
		queryCtx, cancel := context.WithTimeout(ctx, timeout)
		defer cancel()
		return answer(queryCtx, accessToken, project, sql, parameters)
	}, func(file string) error {
		return fillScenarios(ctx, file, project, strings.ToLower(rand.Text()), timeout, transport.call)
	})
}

func fillAnswers(ctx context.Context, path string, query func(string, []json.RawMessage) (string, error), scenarios func(string) error) error {
	paths, err := caseFiles(path)
	if err != nil {
		return err
	}
	var failures []error
	for _, file := range paths {
		if ctx.Err() != nil {
			failures = append(failures, ctx.Err())
			break
		}
		if strings.HasSuffix(file, ".api.json") {
			if scenarios == nil {
				err = fmt.Errorf("API scenario runner is not configured for %s", file)
			} else {
				err = scenarios(file)
			}
		} else {
			err = fillFile(file, query)
		}
		if err != nil {
			failures = append(failures, err)
		}
	}
	return errors.Join(failures...)
}

func fillFile(path string, query func(string, []json.RawMessage) (string, error)) error {
	content, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	lines := strings.Split(string(content), "\n")
	var failures []error
	var sql []string
	var parameters []json.RawMessage
	for i, text := range lines {
		switch {
		case text == "?>":
			got, err := query(strings.Join(sql, "\n"), parameters)
			if err != nil {
				failures = append(failures, fmt.Errorf("%s:%d: %w", path, i+1, err))
			} else {
				lines[i] = "?> " + got
			}
			sql, parameters = nil, nil
		case strings.HasPrefix(text, "?> "):
			sql, parameters = nil, nil
		case strings.HasPrefix(text, "param> "):
			parameter := json.RawMessage(strings.TrimPrefix(text, "param> "))
			if !json.Valid(parameter) {
				failures = append(failures, fmt.Errorf("%s:%d: invalid parameter JSON", path, i+1))
			}
			parameters = append(parameters, parameter)
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
