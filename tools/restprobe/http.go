package main

import (
	"encoding/json"
	"io"
	"net/http"
	"strings"
)

type response struct {
	status int
	body   []byte
}

func send(url, method, path, body string) (response, error) {
	var reader io.Reader
	if body != "" {
		reader = strings.NewReader(body)
	}
	request, err := http.NewRequest(method, url+path, reader)
	if err != nil {
		return response{}, err
	}
	if body != "" {
		request.Header.Set("Content-Type", "application/json")
	}
	result, err := http.DefaultClient.Do(request)
	if err != nil {
		return response{}, err
	}
	defer result.Body.Close()
	data, err := io.ReadAll(result.Body)
	return response{result.StatusCode, data}, err
}

// responseError returns the error a JSON response reports: an HTTP error, or a job's or query's
// error in a successful response, as BigQuery reports those.
func responseError(body map[string]json.RawMessage) (string, bool) {
	type message struct {
		Message string `json:"message"`
	}
	var single message
	if raw, ok := body["error"]; ok {
		json.Unmarshal(raw, &single)
		return single.Message, true
	}
	if raw, ok := body["status"]; ok {
		var status struct {
			ErrorResult *message `json:"errorResult"`
		}
		if json.Unmarshal(raw, &status) == nil && status.ErrorResult != nil {
			return status.ErrorResult.Message, true
		}
	}
	if raw, ok := body["errors"]; ok {
		var errors []message
		if json.Unmarshal(raw, &errors) == nil && len(errors) > 0 {
			return errors[0].Message, true
		}
	}
	if raw, ok := body["insertErrors"]; ok {
		var insertErrors []struct {
			Errors []message `json:"errors"`
		}
		json.Unmarshal(raw, &insertErrors)
		if len(insertErrors) > 0 && len(insertErrors[0].Errors) > 0 {
			return insertErrors[0].Errors[0].Message, true
		}
		return "", true
	}
	return "", false
}

const unsupportedPrefix = "The emulator does not support "
