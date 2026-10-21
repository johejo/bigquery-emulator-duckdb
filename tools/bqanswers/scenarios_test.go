package main

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"
)

func TestExpandJSON(t *testing.T) {
	vars := map[string]any{
		"project": "quoted\"\\project", "n": json.Number("9007199254740993"),
		"body": map[string]any{"value": "${n}"}, "recursive": "${recursive}",
	}
	for _, tc := range []struct {
		name    string
		input   any
		want    any
		failure bool
	}{
		{"typed body", "${body}", map[string]any{"value": json.Number("9007199254740993")}, false},
		{"inline", []any{"projects/${project}", "number ${n}"}, []any{"projects/quoted\"\\project", "number 9007199254740993"}, false},
		{"unknown", "${missing}", nil, true},
		{"object in string", "prefix${body}", nil, true},
		{"cycle", "${recursive}", nil, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			got, err := expand(tc.input, vars)
			if (err != nil) != tc.failure || (err == nil && !reflect.DeepEqual(got, tc.want)) {
				t.Fatalf("expand = %#v, %v; want %#v, failure %v", got, err, tc.want, tc.failure)
			}
		})
	}
}

func TestJSONPointer(t *testing.T) {
	tree := map[string]any{"a/b": map[string]any{"~": []any{nil, "value"}}}
	for _, tc := range []struct {
		path             string
		want             any
		present, invalid bool
	}{
		{"/a~1b/~0/0", nil, true, false}, {"/a~1b/~0/1", "value", true, false},
		{"/missing", nil, false, false}, {"/a~1b/~0/01", nil, false, false},
		{"not-a-pointer", nil, false, true}, {"/missing/~2", nil, false, true},
	} {
		got, present, err := pointer(tree, tc.path)
		if !reflect.DeepEqual(got, tc.want) || present != tc.present || (err != nil) != tc.invalid {
			t.Errorf("pointer(%q) = %v, %v, %v", tc.path, got, present, err)
		}
	}
}

func writeScenarioFixture(t *testing.T, file scenarioFile) string {
	t.Helper()
	data, err := json.Marshal(file)
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "cases.api.json")
	if err := os.WriteFile(path, data, 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

func restStep(name string) scenarioStep {
	return scenarioStep{Name: name, REST: &restRequest{Method: "GET", Path: "/bigquery/v2/projects/${project}"}}
}

func TestScenarioResumeCleanupAndErrors(t *testing.T) {
	setup := restStep("setup")
	setup.Bind = map[string]string{"resource": "/body/resource"}
	observe := restStep("observe")
	observe.Observe = map[string]string{"null": "/body/null", "missing": "/body/missing", "number": "/body/number"}
	cleanup := restStep("cleanup")
	path := writeScenarioFixture(t, scenarioFile{Version: 1, Cases: []scenarioCase{
		{Name: "answered", Steps: []scenarioStep{observe}, Answer: json.RawMessage(`{"old":true}`)},
		{Name: "success", Steps: []scenarioStep{setup, observe}, Cleanup: []scenarioStep{cleanup}},
		{Name: "transport_failure", Steps: []scenarioStep{observe}, Cleanup: []scenarioStep{cleanup}},
		{Name: "rejected", Steps: []scenarioStep{observe, restStep("never")}, Cleanup: []scenarioStep{cleanup}},
	}})
	failure := errors.New("network unavailable")
	var calls []string
	call := func(_ context.Context, step scenarioStep, vars map[string]any) (scenarioResponse, error) {
		name := vars["case"].(string)
		calls = append(calls, name+"/"+step.Name)
		if step.Name == "cleanup" {
			return scenarioResponse{}, nil
		}
		switch name {
		case "transport_failure":
			return scenarioResponse{}, failure
		case "rejected":
			return scenarioResponse{Failed: true, Value: map[string]any{"status": 400, "body": "invalid"}}, nil
		}
		if step.Name == "observe" && vars["resource"] != "captured" {
			t.Fatalf("binding = %v", vars)
		}
		return scenarioResponse{Value: map[string]any{"body": map[string]any{"resource": "captured", "null": nil, "number": json.Number("9007199254740993")}}}, nil
	}
	err := fillScenarios(t.Context(), path, "project", "run", time.Second, call)
	if !errors.Is(err, failure) {
		t.Fatalf("error = %v", err)
	}
	want := []string{"success/setup", "success/observe", "success/cleanup", "transport_failure/observe", "transport_failure/cleanup", "rejected/observe", "rejected/cleanup"}
	if !reflect.DeepEqual(calls, want) {
		t.Fatalf("calls = %v, want %v", calls, want)
	}
	file, err := readScenarios(path)
	if err != nil {
		t.Fatal(err)
	}
	var old map[string]bool
	if err := json.Unmarshal(file.Cases[0].Answer, &old); err != nil {
		t.Fatal(err)
	}
	if !old["old"] || answered(file.Cases[2].Answer) || !answered(file.Cases[3].Answer) {
		t.Fatalf("answers = %+v", file.Cases)
	}
	var result observation
	if err := json.Unmarshal(file.Cases[1].Answer, &result); err != nil {
		t.Fatal(err)
	}
	got := result.Steps["observe"].(map[string]any)
	if _, present := got["missing"]; present {
		t.Fatalf("missing value was recorded: %v", got)
	}
	if value, present := got["null"]; !present || value != nil {
		t.Fatalf("null = %v, %v", value, present)
	}
	if !strings.Contains(string(file.Cases[1].Answer), "9007199254740993") {
		t.Fatal("large integer lost precision")
	}
	info, err := os.Stat(path)
	if err != nil || info.Mode().Perm() != 0o600 {
		t.Fatalf("file permissions: %v, %v", info, err)
	}
	calls = nil
	if err := fillScenarios(t.Context(), path, "project", "newrun", time.Second, func(_ context.Context, step scenarioStep, vars map[string]any) (scenarioResponse, error) {
		calls = append(calls, vars["case"].(string)+"/"+step.Name)
		return scenarioResponse{Value: map[string]any{}}, nil
	}); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(calls, []string{"transport_failure/observe", "transport_failure/cleanup"}) {
		t.Fatalf("resume calls = %v", calls)
	}
}

func TestScenarioCleanupAfterCancellation(t *testing.T) {
	observe := restStep("observe")
	observe.Observe = map[string]string{"response": ""}
	path := writeScenarioFixture(t, scenarioFile{Version: 1, Cases: []scenarioCase{{Name: "cancel", Steps: []scenarioStep{observe}, Cleanup: []scenarioStep{restStep("cleanup")}}}})
	ctx, cancel := context.WithCancel(t.Context())
	cleaned := false
	err := fillScenarios(ctx, path, "project", "run", time.Second, func(ctx context.Context, step scenarioStep, _ map[string]any) (scenarioResponse, error) {
		if step.Name == "observe" {
			cancel()
			return scenarioResponse{}, ctx.Err()
		}
		if ctx.Err() != nil {
			t.Fatalf("cleanup inherited cancellation: %v", ctx.Err())
		}
		cleaned = true
		return scenarioResponse{}, nil
	})
	if !errors.Is(err, context.Canceled) || !cleaned {
		t.Fatalf("error = %v, cleaned = %v", err, cleaned)
	}
}

func TestScenarioRejectsInvalidFilesBeforeCalls(t *testing.T) {
	observe := restStep("observe")
	observe.Observe = map[string]string{"response": ""}
	for _, mutate := range []func(*scenarioFile){
		func(f *scenarioFile) { f.Version = 99 },
		func(f *scenarioFile) { f.Cases[0].Steps[0].Observe["invalid"] = "/missing/~2" },
		func(f *scenarioFile) {
			f.Cases[0].Steps[0].REST.Method = "POST"
			f.Cases[0].Steps[0].Until = map[string]any{"/state": "DONE"}
		},
		func(f *scenarioFile) { f.Cases[0].Steps[0].REST.Path = "https://another-host/" },
		func(f *scenarioFile) { f.Cases[0].Vars = map[string]any{"project": "override"} },
	} {
		step := observe
		step.Observe = map[string]string{"response": ""}
		step.REST = &restRequest{Method: "GET", Path: "/projects/${project}"}
		file := scenarioFile{Version: 1, Cases: []scenarioCase{{Name: "test", Steps: []scenarioStep{step}}}}
		mutate(&file)
		path := writeScenarioFixture(t, file)
		if err := fillScenarios(t.Context(), path, "p", "r", time.Second, func(context.Context, scenarioStep, map[string]any) (scenarioResponse, error) {
			t.Fatal("request sent for invalid file")
			return scenarioResponse{}, nil
		}); err == nil {
			t.Fatal("invalid file accepted")
		}
	}
}

func TestScenarioPollDeadline(t *testing.T) {
	step := restStep("job")
	step.Until = map[string]any{"/body/state": "DONE"}
	ctx, cancel := context.WithTimeout(t.Context(), 10*time.Millisecond)
	defer cancel()
	_, err := pollScenario(ctx, step, nil, func(context.Context, scenarioStep, map[string]any) (scenarioResponse, error) {
		return scenarioResponse{Value: map[string]any{"body": map[string]any{"state": "RUNNING"}}}, nil
	})
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("poll = %v", err)
	}
}

func TestScenarioTemplateAndOfflineValidation(t *testing.T) {
	step := restStep("observe")
	step.REST.Path += "/${captured}"
	step.Observe = map[string]string{"value": "/body/value"}
	setup := restStep("setup")
	setup.Bind = map[string]string{"captured": "/body/resource"}
	path := writeScenarioFixture(t, scenarioFile{Version: 1,
		Templates: map[string]scenarioTemplate{"read": {Steps: []scenarioStep{setup, step}}},
		Cases:     []scenarioCase{{Name: "templated", Template: "read"}},
	})
	// No credentials or network are needed for validation, even when a template references a
	// response binding whose value will only be known at runtime.
	t.Setenv("PATH", t.TempDir())
	if err := run("project", path, true, time.Second); err != nil {
		t.Fatal(err)
	}
	if err := fillScenarios(t.Context(), path, "project", "run", time.Second, func(_ context.Context, step scenarioStep, vars map[string]any) (scenarioResponse, error) {
		if step.Name == "observe" && vars["captured"] != "bound" {
			t.Fatalf("binding = %v", vars)
		}
		return scenarioResponse{Value: map[string]any{"body": map[string]any{"resource": "bound", "value": "answer"}}}, nil
	}); err != nil {
		t.Fatal(err)
	}
	file, err := readScenarios(path)
	if err != nil {
		t.Fatal(err)
	}
	if file.Cases[0].Template != "read" || file.Cases[0].Steps != nil || !answered(file.Cases[0].Answer) {
		t.Fatalf("template answer was not retained: %+v", file.Cases[0])
	}
	file.Cases[0].Template = "missing"
	path = writeScenarioFixture(t, *file)
	if err := run("project", path, true, time.Second); err == nil {
		t.Fatal("unknown template accepted")
	}
}

func TestScenarioVariableScopes(t *testing.T) {
	for _, vars := range []map[string]any{{"value": "${unknown}"}, {"value": "${value}"}} {
		step := restStep("read")
		step.REST.Body = "${value}"
		step.Observe = map[string]string{"value": ""}
		path := writeScenarioFixture(t, scenarioFile{Version: 1, Vars: vars, Cases: []scenarioCase{{Name: "bad", Steps: []scenarioStep{step}}}})
		if _, err := readScenarios(path); err == nil {
			t.Fatal("invalid variable references accepted")
		}
	}
}

func TestCleanupFailureKeepsAnswer(t *testing.T) {
	for _, code := range []int{404, 500} {
		step := restStep("observe")
		step.Observe = map[string]string{"value": "/body/value"}
		cleanup := restStep("delete")
		cleanup.REST.Method = "DELETE"
		path := writeScenarioFixture(t, scenarioFile{Version: 1, Cases: []scenarioCase{{Name: "cleanup", Steps: []scenarioStep{step}, Cleanup: []scenarioStep{cleanup}}}})
		err := fillScenarios(t.Context(), path, "p", "run", time.Second, func(_ context.Context, step scenarioStep, _ map[string]any) (scenarioResponse, error) {
			if step.Name == "delete" {
				return scenarioResponse{Failed: true, Value: map[string]any{"status": code}}, nil
			}
			return scenarioResponse{Value: map[string]any{"body": map[string]any{"value": "answer"}}}, nil
		})
		if (err != nil) != (code == 500) {
			t.Fatalf("cleanup HTTP %d: %v", code, err)
		}
		file, readErr := readScenarios(path)
		if readErr != nil || !answered(file.Cases[0].Answer) {
			t.Fatalf("answer lost: %v", readErr)
		}
	}
}

func TestRepositoryScenarios(t *testing.T) {
	paths, err := filepath.Glob("../../tests/e2e/goclient/testdata/unverified/*.api.json")
	if err != nil {
		t.Fatal(err)
	}
	for _, path := range paths {
		if _, err := readScenarios(path); err != nil {
			t.Error(err)
		}
	}
}
