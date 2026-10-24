package bqscenario

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

const yamlFixture = `# This comment and formatting must survive collection.
version: 1
vars:
  dataset: bqa_${run}_${case}
  integer: 9007199254740993
  decimal: 0.12345678901234567890123456789
  text: on
  date: 2026-10-24
templates:
  read:
    steps:
      - name: read
        rest:
          method: POST
          path: /projects/${project}
          body:
            query: |
              SELECT 1
              -- a second line
            value: ${integer}
            decimal: ${decimal}
        observe:
          value: /body/value
          nullable: /body/nullable
          missing: /body/missing
    cleanup:
      - name: delete
        rest: {method: DELETE, path: "/projects/${project}/datasets/${dataset}"}
cases:
  - name: example
    template: read
    expect: {"read": {"value": 9007199254740993, "nullable": null}}
`

func yamlFixturePath(t *testing.T, data string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "cases.api.yaml")
	if err := os.WriteFile(path, []byte(data), 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

func fixtureCall(_ context.Context, step Step, _ map[string]any) (Response, error) {
	if step.Name == "delete" {
		return Response{Failed: true, Value: map[string]any{"status": json.Number("404")}}, nil
	}
	return Response{Value: map[string]any{"body": map[string]any{"value": json.Number("9007199254740993"), "nullable": nil}}}, nil
}

func TestYAMLPrecisionAndMultiline(t *testing.T) {
	file, err := Read(yamlFixturePath(t, yamlFixture))
	if err != nil {
		t.Fatal(err)
	}
	c, err := file.resolveCase(file.Cases[0])
	if err != nil {
		t.Fatal(err)
	}
	request, err := expandedRequest(*c.Steps[0].REST, file.variables(c, "test", "run"))
	if err != nil {
		t.Fatal(err)
	}
	body := request.Body.(map[string]any)
	if body["value"] != json.Number("9007199254740993") || body["decimal"] != json.Number("0.12345678901234567890123456789") {
		t.Fatalf("numeric precision lost: %#v", body)
	}
	if body["query"] != "SELECT 1\n-- a second line\n" || file.Vars["text"] != "on" || file.Vars["date"] != "2026-10-24" {
		t.Fatalf("YAML strings: %#v, %#v", body, file.Vars)
	}
}

func TestYAMLCollectionSidecarResumeAndVerify(t *testing.T) {
	path := yamlFixturePath(t, yamlFixture)
	if err := Collect(t.Context(), path, "project", "run", time.Second, fixtureCall); err != nil {
		t.Fatal(err)
	}
	original, err := os.ReadFile(path)
	if err != nil || string(original) != yamlFixture {
		t.Fatalf("scenario modified: %s, %v", original, err)
	}
	data, err := os.ReadFile(AnswersPath(path))
	if err != nil || !strings.Contains(string(data), "9007199254740993") || !strings.Contains(string(data), `"project": "project"`) {
		t.Fatalf("sidecar: %s, %v", data, err)
	}
	if err := Collect(t.Context(), path, "project", "newrun", time.Second, func(context.Context, Step, map[string]any) (Response, error) {
		t.Fatal("answered case replayed")
		return Response{}, nil
	}); err != nil {
		t.Fatal(err)
	}
	file, err := Read(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(file.Cases[0].Answer) != 0 {
		t.Fatal("verification loaded a collected answer")
	}
	if err := file.Verify(t.Context(), file.Cases[0], "local", "localrun", time.Second, fixtureCall); err != nil {
		t.Fatal(err)
	}
	after, _ := os.ReadFile(AnswersPath(path))
	if string(after) != string(data) {
		t.Fatal("verification modified observations")
	}
}

func TestVerifyMismatchAndCleanup(t *testing.T) {
	file, err := Read(yamlFixturePath(t, yamlFixture))
	if err != nil {
		t.Fatal(err)
	}
	failure := errors.New("cleanup failed")
	for _, want := range []string{
		`{"read":{"value":9007199254740992,"nullable":null}}`,
		`{"read":{"value":9007199254740993}}`,
		`{"read":{"value":9007199254740993,"nullable":null,"missing":null}}`,
	} {
		c := file.Cases[0]
		c.Expect = json.RawMessage(want)
		cleaned := false
		err := file.Verify(t.Context(), c, "p", "r", time.Second, func(ctx context.Context, step Step, vars map[string]any) (Response, error) {
			if step.Name == "delete" {
				cleaned = true
				return Response{}, failure
			}
			return fixtureCall(ctx, step, vars)
		})
		if !cleaned || !errors.Is(err, failure) || !strings.Contains(err.Error(), "observations differ") {
			t.Fatalf("verify: %v, cleanup=%v", err, cleaned)
		}
	}
	c := file.Cases[0]
	c.Expect = nil
	if err := file.Verify(t.Context(), c, "p", "r", time.Second, fixtureCall); err == nil {
		t.Fatal("missing expectation accepted")
	}
}

func TestYAMLRejectsInvalidData(t *testing.T) {
	for _, data := range []string{
		yamlFixture + "\n---\nversion: 1\n",
		strings.Replace(yamlFixture, "version: 1", "version: 1\nversion: 1", 1),
		strings.Replace(yamlFixture, "version: 1", "version: 1\nunknown: true", 1),
		strings.Replace(yamlFixture, "integer: 9007199254740993", "integer: .inf", 1),
		strings.Replace(yamlFixture, "integer: 9007199254740993", "integer: &value 1", 1),
		strings.Replace(yamlFixture, "integer: 9007199254740993", "123: 1", 1),
		strings.Replace(yamlFixture, "template: read", "template: read\n    answer: {}", 1),
		strings.Replace(yamlFixture, "template: read", "template: read\n    expect: []", 1),
	} {
		if _, err := Read(yamlFixturePath(t, data)); err == nil {
			t.Errorf("invalid scenario accepted:\n%s", data)
		}
	}
}

func TestExpectedFailureContinuesAndSelectsFields(t *testing.T) {
	path := yamlFixturePath(t, `version: 1
cases:
  - name: rejection
    steps:
      - name: rejected
        expectFailure: true
        rest: {method: POST, path: "/projects/${project}"}
        observe: {status: /status, message: /body/message}
      - name: after
        rest: {method: GET, path: "/projects/${project}"}
        observe: {status: /status}
    expect:
      rejected: {status: 400, message: invalid}
      after: {status: 200}
`)
	file, err := Read(path)
	if err != nil {
		t.Fatal(err)
	}
	call := func(_ context.Context, step Step, _ map[string]any) (Response, error) {
		if step.Name == "rejected" {
			return Response{Failed: true, Value: map[string]any{"status": 400, "body": map[string]any{"message": "invalid", "dynamic": "not compared"}}}, nil
		}
		return Response{Value: map[string]any{"status": 200}}, nil
	}
	if err := file.Verify(t.Context(), file.Cases[0], "p", "r", time.Second, call); err != nil {
		t.Fatal(err)
	}
	if err := file.Verify(t.Context(), file.Cases[0], "p", "r", time.Second, func(context.Context, Step, map[string]any) (Response, error) {
		return Response{Value: map[string]any{"status": 200}}, nil
	}); err == nil {
		t.Fatal("unexpected success accepted")
	}
}

func TestInvalidSidecarDoesNotAffectVerification(t *testing.T) {
	for _, sidecar := range []string{
		`{"version":99,"cases":{}}`,
		`{"version":1,"cases":{"unknown":{"project":"p","run":"r","steps":{}}}}`,
		`{"version":1,"cases":{"example":{}}}`,
		`{"version":1,"cases":{"example":[]}}`,
		`{"version":1,"cases":{}} {}`,
	} {
		path := yamlFixturePath(t, yamlFixture)
		if err := os.WriteFile(AnswersPath(path), []byte(sidecar), 0o600); err != nil {
			t.Fatal(err)
		}
		if err := ValidateCollection(path); err == nil {
			t.Fatalf("invalid sidecar accepted: %s", sidecar)
		}
		file, err := Read(path)
		if err != nil {
			t.Fatal(err)
		}
		if err := file.Verify(t.Context(), file.Cases[0], "p", "r", time.Second, fixtureCall); err != nil {
			t.Fatal(err)
		}
	}
}
