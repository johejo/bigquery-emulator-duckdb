package bqscenario

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
	"time"
)

func TestObservationMatchers(t *testing.T) {
	for _, tc := range []struct {
		got, want     string
		present, pass bool
	}{
		{`null`, `{"$missing":true}`, false, true}, {`null`, `{"$missing":true}`, true, false},
		{`null`, `{"$oneOf":[{"$missing":true},""]}`, false, true}, {`null`, `{"$oneOf":[{"$missing":true},""]}`, true, false},
		{`"context: bad input"`, `{"$contains":"bad input"}`, true, true},
		{`"other"`, `{"$contains":"bad input"}`, true, false},
		{`"9007199254740993"`, `{"$gt":"9007199254740992"}`, true, true},
		{`"9007199254740992"`, `{"$gt":"9007199254740993"}`, true, false},
		{`{"x":1,"ignored":2}`, `{"$subset":{"x":1}}`, true, true},
		{`{"x":1,"extra":2}`, `{"x":1}`, true, false},
		{`{"x":1}`, `{"x":1,"absent":{"$missing":true}}`, true, true},
		{`[]`, `[1]`, true, false},
		{`1`, `{"$typo":true}`, true, false},
	} {
		var got, want any
		if err := decodeJSON([]byte(tc.got), &got); err != nil {
			t.Fatal(err)
		}
		if err := decodeJSON([]byte(tc.want), &want); err != nil {
			t.Fatal(err)
		}
		if err := match(got, tc.present, want, "test"); (err == nil) != tc.pass {
			t.Errorf("match %s, %s: %v", tc.got, tc.want, err)
		}
	}
}

func TestStepExpectationsBindingsAndCollection(t *testing.T) {
	path := yamlFixturePath(t, `version: 1
stepTemplates:
  read:
    rest: {method: GET, path: "/projects/${project}/${value}"}
    observe: {value: /body/value}
cases:
  - name: example
    steps:
      - name: first
        use: read
        vars: {value: original}
        bind: {previous: /body/value}
        expect: {value: original}
      - name: second
        use: read
        vars: {value: changed}
        expect: {value: "${previous}"}
`)
	file, err := Read(path)
	if err != nil {
		t.Fatal(err)
	}
	call := func(_ context.Context, step Step, vars map[string]any) (Response, error) {
		return Response{Value: map[string]any{"body": map[string]any{"value": vars["value"]}}}, nil
	}
	if err := file.Verify(t.Context(), file.Cases[0], "p", "r", time.Second, call); err == nil || !strings.Contains(err.Error(), "second") {
		t.Fatalf("step mismatch: %v", err)
	}
	if err := Collect(t.Context(), path, "p", "r", time.Second, call); err != nil {
		t.Fatalf("collection applied expectations: %v", err)
	}
	failure := func(context.Context, Step, map[string]any) (Response, error) {
		return Response{Failed: true, Value: map[string]any{"status": json.Number("400")}}, nil
	}
	if err := file.Verify(t.Context(), file.Cases[0], "p", "r", time.Second, failure); err == nil {
		t.Fatal("unexpected rejection passed with only step expectations")
	}
}

func TestSchemaComparisonPreservesSDKProperties(t *testing.T) {
	for _, tc := range []struct {
		actual string
		pass   bool
	}{
		{`[{"name":"id","type":"INTEGER"}]`, true},
		{`[{"name":"id","type":"INTEGER","mode":"NULLABLE","precision":"0","fields":[]}]`, true},
		{`[{"name":"id","type":"INTEGER","description":"changed"}]`, false},
		{`[{"name":"id","type":"INTEGER","mode":"REQUIRED"}]`, false},
		{`[{"name":"id","type":"INTEGER","defaultValueExpression":"1"}]`, false},
		{`[{"name":"id","type":"INTEGER"},{"name":"extra","type":"STRING"}]`, false},
	} {
		var got, want any
		if err := decodeJSON([]byte(tc.actual), &got); err != nil {
			t.Fatal(err)
		}
		if err := decodeJSON([]byte(`{"$schema":[{"name":"id","type":"INTEGER"}]}`), &want); err != nil {
			t.Fatal(err)
		}
		if err := match(got, true, want, "schema"); (err == nil) != tc.pass {
			t.Errorf("schema %s: %v", tc.actual, err)
		}
	}
}

func TestCaseExpectCanCompareLegacyObservedRejection(t *testing.T) {
	c := Case{Name: "legacy", Steps: []Step{{Name: "read", REST: &RESTRequest{Method: "GET", Path: "/"}, Observe: map[string]string{"status": "/status"}}}, Expect: json.RawMessage(`{"read":{"error":{"status":400}}}`)}
	f := File{Version: 1, Cases: []Case{c}}
	err := f.Verify(t.Context(), c, "p", "r", time.Second, func(context.Context, Step, map[string]any) (Response, error) {
		return Response{Failed: true, Value: map[string]any{"status": json.Number("400")}}, nil
	})
	if err != nil {
		t.Fatal(err)
	}
}
