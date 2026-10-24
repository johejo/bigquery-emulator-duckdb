package bqscenario

import (
	"context"
	"encoding/json"
	"reflect"
	"strings"
	"testing"
	"time"
)

const groupFixture = `version: 1
stepTemplates:
  read:
    rest: {method: GET, path: "/resources/${id}"}
    observe: {value: /body/value}
    expect: {value: "${input}"}
stepGroups:
  remove:
    - name: all
      rest: {method: DELETE, path: "/resources/${label}"}
  roundtrip:
    - name: create
      rest: {method: POST, path: "/resources/${label}", body: {value: "${input}"}}
      bind: {id: /body/id}
    - name: read
      use: read
cases:
  - name: repeated
    steps:
      - name: first
        group: roundtrip
        vars: {label: first, input: 9007199254740993}
      - name: second
        group: roundtrip
        vars: {label: second, input: 0.12345678901234567890123456789}
      - name: outside
        rest: {method: GET, path: "/resources/${first_id}/${second_id}"}
        observe: {}
        expect: {}
    cleanup:
      - name: delete
        group: remove
        vars: {label: all}
`

func TestRepeatedGroupsVerifyAndCollect(t *testing.T) {
	for _, collect := range []bool{false, true} {
		t.Run(map[bool]string{false: "verify", true: "collect"}[collect], func(t *testing.T) {
			data := groupFixture
			if collect {
				// Collection still records independent observations, ignoring assertions.
				data = strings.Replace(data, `expect: {value: "${input}"}`, `expect: {value: wrong}`, 1)
			}
			path := yamlFixturePath(t, data)
			file, err := Read(path)
			if err != nil {
				t.Fatal(err)
			}
			resources := map[string]any{}
			requests := []string{}
			call := func(_ context.Context, step Step, vars map[string]any) (Response, error) {
				request, err := expandedRequest(*step.REST, vars)
				if err != nil {
					return Response{}, err
				}
				requests = append(requests, request.Method+" "+request.Path)
				switch request.Method {
				case "POST":
					resources[request.Path] = request.Body.(map[string]any)["value"]
					return Response{Value: map[string]any{"body": map[string]any{"id": strings.TrimPrefix(request.Path, "/resources/")}}}, nil
				case "GET":
					if value, ok := resources[request.Path]; ok {
						return Response{Value: map[string]any{"body": map[string]any{"value": value}}}, nil
					}
				}
				return Response{Value: map[string]any{}}, nil
			}
			if collect {
				err = Collect(t.Context(), path, "p", "r", time.Second, call)
			} else {
				err = file.Verify(t.Context(), file.Cases[0], "p", "r", time.Second, call)
			}
			if err != nil {
				t.Fatal(err)
			}
			want := []string{"POST /resources/first", "GET /resources/first", "POST /resources/second", "GET /resources/second", "GET /resources/first/second", "DELETE /resources/all"}
			if !reflect.DeepEqual(requests, want) {
				t.Fatalf("requests = %v", requests)
			}
			if resources["/resources/first"] != json.Number("9007199254740993") || resources["/resources/second"] != json.Number("0.12345678901234567890123456789") {
				t.Fatalf("payload precision lost: %v", resources)
			}
		})
	}
}

func TestGroupValidation(t *testing.T) {
	for _, change := range []struct{ old, new string }{
		{"group: roundtrip", "group: missing"},
		{"group: roundtrip", "group: roundtrip\n        expect: {}"},
		{"name: create\n      rest:", "name: create\n      group: roundtrip\n      rest:"},
		{"bind: {id: /body/id}", "bind: {project: /body/id}"},
		{"name: first\n        group:", "name: \"\"\n        group:"},
		{"/resources/${first_id}/${second_id}", "/resources/${label}"},
		{"/resources/${first_id}/${second_id}", "/resources/${id}"},
	} {
		data := strings.Replace(groupFixture, change.old, change.new, 1)
		if data == groupFixture {
			t.Fatalf("bad test substitution: %s", change.old)
		}
		if _, err := Read(yamlFixturePath(t, data)); err == nil {
			t.Errorf("invalid group accepted: %s", change.new)
		}
	}
}

func TestGroupFailureRunsCleanup(t *testing.T) {
	data := strings.Replace(groupFixture, `expect: {value: "${input}"}`, `expect: {value: wrong}`, 1)
	file, err := Read(yamlFixturePath(t, data))
	if err != nil {
		t.Fatal(err)
	}
	requests := []string{}
	err = file.Verify(t.Context(), file.Cases[0], "p", "r", time.Second, func(_ context.Context, s Step, vars map[string]any) (Response, error) {
		request, e := expandedRequest(*s.REST, vars)
		if e != nil {
			return Response{}, e
		}
		requests = append(requests, request.Method+" "+request.Path)
		return Response{Value: map[string]any{"body": map[string]any{"id": "first", "value": json.Number("9007199254740993")}}}, nil
	})
	if err == nil || !strings.Contains(err.Error(), "first_read") {
		t.Fatalf("group failure = %v", err)
	}
	if want := []string{"POST /resources/first", "GET /resources/first", "DELETE /resources/all"}; !reflect.DeepEqual(requests, want) {
		t.Fatalf("failure requests = %v", requests)
	}
}

func TestGroupExpectedRejectionContinues(t *testing.T) {
	file, err := Read(yamlFixturePath(t, `version: 1
stepGroups:
  reject:
    - name: rejected
      rest: {method: POST, path: /unsupported}
      expectFailure: true
      observe: {}
      expect: {}
cases:
  - name: rejection
    steps:
      - {name: first, group: reject}
      - name: after
        rest: {method: GET, path: /after}
        observe: {}
        expect: {}
`))
	if err != nil {
		t.Fatal(err)
	}
	calls := []string{}
	err = file.Verify(t.Context(), file.Cases[0], "p", "r", time.Second, func(_ context.Context, s Step, _ map[string]any) (Response, error) {
		calls = append(calls, s.REST.Path)
		return Response{Failed: s.REST.Method == "POST", Value: map[string]any{}}, nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(calls, []string{"/unsupported", "/after"}) {
		t.Fatalf("calls = %v", calls)
	}
}
