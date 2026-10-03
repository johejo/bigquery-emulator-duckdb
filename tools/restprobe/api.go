package main

import (
	"bytes"
	_ "embed"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

//go:embed api_preamble.md
var apiPreamble string

const (
	apiDocs   = "https://cloud.google.com/bigquery/docs/reference/rest/v2"
	apiPrefix = "/bigquery/v2/"
)

type apiRequest struct {
	method, path, body string
}

type apiMethod struct {
	id, httpMethod, flatPath string
	hasBody                  bool
	// What paths the method is served at: its path below the API prefix and its media upload
	// paths.
	patterns []*regexp.Regexp
	requests []apiRequest
	note     string
}

type apiResource struct {
	name    string
	methods []*apiMethod
}

// parseRequest parses "METHOD PATH [BODY]". $DIR in the path or body stands for the directory the
// file directives write to, which each probe has a copy of.
func parseRequest(text string) (apiRequest, bool) {
	method, rest, _ := strings.Cut(text, " ")
	path, body, _ := strings.Cut(strings.TrimLeft(rest, " "), " ")
	body = strings.TrimLeft(body, " \t")
	return apiRequest{method, path, body}, strings.HasPrefix(path, "/")
}

func (r apiRequest) in(dir string) apiRequest {
	return apiRequest{r.method, strings.ReplaceAll(r.path, "$DIR", dir),
		strings.ReplaceAll(r.body, "$DIR", dir)}
}

var pathVariable = regexp.MustCompile(`\{([^}]+)\}`)

// pathPattern matches the paths a discovery path template such as "projects/{projectsId}/jobs"
// stands for.
func pathPattern(path string) *regexp.Regexp {
	var pattern strings.Builder
	last := 0
	for _, match := range pathVariable.FindAllStringIndex(path, -1) {
		pattern.WriteString(regexp.QuoteMeta(path[last:match[0]]) + "[^/]+")
		last = match[1]
	}
	pattern.WriteString(regexp.QuoteMeta(path[last:]))
	return regexp.MustCompile("^" + pattern.String() + "$")
}

// generatedRequest is the request the probe sends for a method the methods file gives none for:
// the method's path with the fixture's identifiers, and an empty body when the method takes one.
func generatedRequest(m *apiMethod) apiRequest {
	ids := map[string]string{"projectsId": "test", "datasetsId": "d", "tablesId": "t",
		"jobsId": "j", "queriesId": "j"}
	path := apiPrefix + pathVariable.ReplaceAllStringFunc(m.flatPath, func(variable string) string {
		if id, ok := ids[variable[1:len(variable)-1]]; ok {
			return id
		}
		return "x"
	})
	body := ""
	if m.hasBody {
		body = "{}"
	}
	return apiRequest{m.httpMethod, path, body}
}

// sendProbe sends request to the emulator at url and classifies the outcome.
func sendProbe(url string, request apiRequest) (probe, error) {
	result, err := send(url, request.method, request.path, request.body)
	if err != nil {
		return probe{}, err
	}
	sent := "`" + request.method + " " + request.path + "`: "
	// An unmatched route gets cpp-httplib's empty 404, never a BigQuery error.
	if result.status == 404 && len(result.body) == 0 {
		return probe{unsupported, ""}, nil
	}
	// Deletes succeed with an empty body.
	if result.status/100 == 2 && len(result.body) == 0 {
		return probe{runs, ""}, nil
	}
	var body map[string]json.RawMessage
	if err := json.Unmarshal(result.body, &body); err != nil {
		return probe{untested, sent + "the response is not JSON"}, nil
	}
	message, failed := responseError(body)
	if !failed {
		if result.status/100 == 2 {
			return probe{runs, ""}, nil
		}
		return probe{untested, sent + "HTTP " + strconv.Itoa(result.status)}, nil
	}
	message = firstLine(message)
	if feature, ok := strings.CutPrefix(message, unsupportedPrefix); ok {
		// The feature, without a hint such as "; set useLegacySql to false".
		feature, _, _ = strings.Cut(feature, "; ")
		return probe{unsupported, feature}, nil
	}
	if result.status >= 500 {
		return probe{broken, sent + message}, nil
	}
	return probe{untested, sent + message}, nil
}

// probeRequest sends request to a fresh emulator holding the fixture, with the files the methods
// file writes in a directory of its own.
func probeRequest(binary string, files map[string]string, setup []apiRequest,
	request apiRequest) (probe, error) {
	dir, err := os.MkdirTemp("", "restprobe")
	if err != nil {
		return probe{}, err
	}
	defer os.RemoveAll(dir)
	for name, content := range files {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(content), 0o644); err != nil {
			return probe{}, err
		}
	}
	emulator, err := startEmulator(binary)
	if err != nil {
		return probe{}, err
	}
	defer emulator.stop()
	for _, step := range setup {
		step = step.in(dir)
		p, err := sendProbe(emulator.url, step)
		if err != nil {
			return probe{}, err
		}
		if p.outcome != runs {
			return probe{}, fmt.Errorf("setup failed: %s %s: %s", step.method, step.path, p.detail)
		}
	}
	return sendProbe(emulator.url, request.in(dir))
}

func readDiscovery(path string) ([]*apiResource, map[string]*apiMethod, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, nil, err
	}
	var discovery struct {
		Resources map[string]struct {
			Methods map[string]struct {
				ID          string          `json:"id"`
				HTTPMethod  string          `json:"httpMethod"`
				FlatPath    string          `json:"flatPath"`
				Request     json.RawMessage `json:"request"`
				MediaUpload *struct {
					Protocols map[string]struct {
						Path string `json:"path"`
					} `json:"protocols"`
				} `json:"mediaUpload"`
			} `json:"methods"`
		} `json:"resources"`
	}
	if err := json.Unmarshal(data, &discovery); err != nil {
		return nil, nil, fmt.Errorf("%s: %w", path, err)
	}
	// Resources and their methods in alphabetical order.
	var resources []*apiResource
	methods := map[string]*apiMethod{}
	for _, name := range sortedNames(discovery.Resources) {
		resource := &apiResource{name: name}
		entries := discovery.Resources[name].Methods
		for _, methodName := range sortedNames(entries) {
			entry := entries[methodName]
			m := &apiMethod{id: entry.ID, httpMethod: entry.HTTPMethod, flatPath: entry.FlatPath,
				hasBody: entry.Request != nil}
			m.patterns = append(m.patterns, pathPattern(apiPrefix+m.flatPath))
			if entry.MediaUpload != nil {
				for _, protocol := range sortedNames(entry.MediaUpload.Protocols) {
					m.patterns = append(m.patterns, pathPattern(entry.MediaUpload.Protocols[protocol].Path))
				}
			}
			methods[m.id] = m
			resource.methods = append(resource.methods, m)
		}
		resources = append(resources, resource)
	}
	return resources, methods, nil
}

func probeAPI(w io.Writer, binary, discoveryPath, path string) error {
	resources, methods, err := readDiscovery(discoveryPath)
	if err != nil {
		return err
	}
	lines, err := readLines(path)
	if err != nil {
		return err
	}
	files := map[string]string{}
	var setup []apiRequest
	for _, l := range lines {
		if l.directive == "setup" {
			request, ok := parseRequest(l.rest)
			if !ok {
				return fmt.Errorf("%s:%d: cannot parse: setup %s", path, l.number, l.rest)
			}
			setup = append(setup, request)
			continue
		}
		name, value, ok := splitNamed(l.rest)
		if !ok || (l.directive != "file" && l.directive != "request" && l.directive != "note") {
			return fmt.Errorf("%s:%d: cannot parse: %s %s", path, l.number, l.directive, l.rest)
		}
		if l.directive == "file" {
			files[name] = value + "\n"
			continue
		}
		m := methods[name]
		if m == nil {
			return fmt.Errorf("%s:%d: not a method in %s: %s", path, l.number, discoveryPath, name)
		}
		if l.directive == "note" {
			if m.note != "" {
				return fmt.Errorf("%s:%d: second note for %s", path, l.number, name)
			}
			m.note = value
			continue
		}
		request, ok := parseRequest(value)
		target, _, _ := strings.Cut(request.path, "?")
		matches := false
		for _, pattern := range m.patterns {
			matches = matches || pattern.MatchString(target)
		}
		if !ok || request.method != m.httpMethod || !matches {
			return fmt.Errorf("%s:%d: not a request for %s: %s", path, l.number, name, value)
		}
		m.requests = append(m.requests, request)
	}

	type job struct {
		method  *apiMethod
		request apiRequest
	}
	var jobs []job
	for _, resource := range resources {
		for _, m := range resource.methods {
			requests := m.requests
			if len(requests) == 0 {
				requests = []apiRequest{generatedRequest(m)}
			}
			for _, request := range requests {
				jobs = append(jobs, job{m, request})
			}
		}
	}
	probes := make([]probe, len(jobs))
	if err := parallel(len(jobs), func(i int) (err error) {
		probes[i], err = probeRequest(binary, files, setup, jobs[i].request)
		return err
	}); err != nil {
		return err
	}
	results := map[*apiMethod][]probe{}
	for i, j := range jobs {
		results[j.method] = append(results[j.method], probes[i])
	}

	totals := map[string]int{}
	var failed []string
	var body bytes.Buffer
	for _, resource := range resources {
		fmt.Fprintf(&body, "## %s\n\n| Method | HTTP request | Status | Notes |\n"+
			"| --- | --- | --- | --- |\n", resource.name)
		for _, m := range resource.methods {
			generated := len(m.requests) == 0
			counts := map[outcome]int{}
			// The features the emulator rejected as unsupported, and the requests that failed.
			rejected := map[string]bool{}
			failures := map[string]bool{}
			for _, p := range results[m] {
				counts[p.outcome]++
				switch {
				case p.outcome == untested && !generated:
					failed = append(failed, m.id+": "+p.detail)
				case p.outcome == unsupported && p.detail != "":
					rejected[p.detail] = true
				case p.outcome != runs && p.outcome != unsupported:
					failures[p.detail] = true
				}
			}
			st := status(counts)
			totals[st]++
			var note []string
			if m.note != "" {
				note = append(note, m.note)
			}
			if len(rejected) > 0 {
				note = append(note, "Rejected as unsupported: "+strings.Join(sortedNames(rejected), ", ")+".")
			}
			for _, failure := range sortedNames(failures) {
				note = append(note, "Fails: "+failure+".")
			}
			// bigquery.jobs.query is documented at jobs/query.
			_, name, _ := strings.Cut(m.id, ".")
			_, page, _ := strings.Cut(name, ".")
			fmt.Fprintf(&body, "| [`%s`](%s/%s/%s) | `%s %s` | %s | %s |\n", name, apiDocs,
				resource.name, page, m.httpMethod, m.flatPath, st, escape(strings.Join(note, " ")))
		}
		body.WriteString("\n")
	}
	// Every request in the methods file is written by hand, so one that fails for a reason other
	// than the emulator's is a mistake in the file.
	if len(failed) > 0 {
		return fmt.Errorf("requests failed:\n%s", strings.Join(failed, "\n"))
	}
	fmt.Fprint(w, apiPreamble)
	writeTotals(w, "Methods", totals)
	fmt.Fprint(w, "\n", strings.TrimSuffix(body.String(), "\n"))
	return nil
}
