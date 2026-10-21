package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"time"
)

// Scenarios keep requests and observations together. Each case has its own resources and cleanup,
// so an unanswered case can be rerun without replaying a case whose answer is already recorded.
type scenarioFile struct {
	Version     int                         `json:"version"`
	Description string                      `json:"description,omitempty"`
	Vars        map[string]any              `json:"vars,omitempty"`
	Templates   map[string]scenarioTemplate `json:"templates,omitempty"`
	Cases       []scenarioCase              `json:"cases"`
}

type scenarioTemplate struct {
	Steps   []scenarioStep `json:"steps"`
	Cleanup []scenarioStep `json:"cleanup,omitempty"`
}

type scenarioCase struct {
	Name        string          `json:"name"`
	Template    string          `json:"template,omitempty"`
	Description string          `json:"description,omitempty"`
	Vars        map[string]any  `json:"vars,omitempty"`
	Steps       []scenarioStep  `json:"steps,omitempty"`
	Cleanup     []scenarioStep  `json:"cleanup,omitempty"`
	Answer      json.RawMessage `json:"answer,omitempty"`
}

type scenarioStep struct {
	Name    string            `json:"name"`
	REST    *restRequest      `json:"rest,omitempty"`
	GRPC    *grpcRequest      `json:"grpc,omitempty"`
	Bind    map[string]string `json:"bind,omitempty"`
	Observe map[string]string `json:"observe,omitempty"`
	// Poll only GET requests: retrying a mutation could duplicate jobs or writes.
	Until map[string]any `json:"until,omitempty"`
}

type restRequest struct {
	Method  string            `json:"method"`
	Path    string            `json:"path"`
	Headers map[string]string `json:"headers,omitempty"`
	Body    any               `json:"body,omitempty"`
	// Upload is inline media for jobs.insert's multipart/related upload, such as CSV or JSON Lines.
	Upload *string `json:"upload,omitempty"`
}

type grpcRequest struct {
	Method   string            `json:"method"`
	Headers  map[string]string `json:"headers,omitempty"`
	Requests []any             `json:"requests"`
}

type observation struct {
	Project string         `json:"project"`
	Run     string         `json:"run"`
	Steps   map[string]any `json:"steps"`
}

type scenarioResponse struct {
	Value  any
	Failed bool
}

type scenarioCall func(context.Context, scenarioStep, map[string]any) (scenarioResponse, error)

var variablePattern = regexp.MustCompile(`\$\{([a-zA-Z_][a-zA-Z_0-9]*)\}`)
var caseNamePattern = regexp.MustCompile(`^[a-zA-Z][a-zA-Z_0-9]*$`)

// expand substitutes whole JSON values without stringifying them. Within a string a variable
// must be scalar; expanding the JSON tree rather than its encoding preserves quoting and numbers.
func expand(value any, vars map[string]any) (any, error) {
	return expandDepth(value, vars, 0)
}

func expandDepth(value any, vars map[string]any, depth int) (any, error) {
	if depth > 64 {
		return nil, errors.New("variable expansion is recursive or too deep")
	}
	switch value := value.(type) {
	case string:
		matches := variablePattern.FindAllStringSubmatchIndex(value, -1)
		if len(matches) == 0 {
			return value, nil
		}
		var result strings.Builder
		last := 0
		for _, match := range matches {
			name := value[match[2]:match[3]]
			replacement, ok := vars[name]
			if !ok {
				return nil, fmt.Errorf("unknown variable %q", name)
			}
			replacement, err := expandDepth(replacement, vars, depth+1)
			if err != nil {
				return nil, err
			}
			if len(matches) == 1 && match[0] == 0 && match[1] == len(value) {
				return replacement, nil
			}
			result.WriteString(value[last:match[0]])
			switch replacement := replacement.(type) {
			case string:
				result.WriteString(replacement)
			case json.Number, bool:
				fmt.Fprint(&result, replacement)
			default:
				return nil, fmt.Errorf("variable %q is not a scalar string substitution", name)
			}
			last = match[1]
		}
		result.WriteString(value[last:])
		return result.String(), nil
	case []any:
		result := make([]any, len(value))
		for i, element := range value {
			expanded, err := expandDepth(element, vars, depth+1)
			if err != nil {
				return nil, err
			}
			result[i] = expanded
		}
		return result, nil
	case map[string]any:
		result := make(map[string]any, len(value))
		for key, element := range value {
			expanded, err := expandDepth(element, vars, depth+1)
			if err != nil {
				return nil, err
			}
			result[key] = expanded
		}
		return result, nil
	default:
		return value, nil
	}
}

// pointer follows RFC 6901. A missing member differs from a JSON null: missing observations are
// omitted from the answer, whereas a member whose value is null remains present with that value.
func pointer(value any, path string) (any, bool, error) {
	if path == "" {
		return value, true, nil
	}
	if !strings.HasPrefix(path, "/") {
		return nil, false, fmt.Errorf("invalid JSON pointer %q", path)
	}
	tokens := strings.Split(path[1:], "/")
	for n, encoded := range tokens {
		var token strings.Builder
		for i := 0; i < len(encoded); i++ {
			if encoded[i] != '~' {
				token.WriteByte(encoded[i])
				continue
			}
			i++
			if i >= len(encoded) || (encoded[i] != '0' && encoded[i] != '1') {
				return nil, false, fmt.Errorf("invalid JSON pointer %q", path)
			}
			if encoded[i] == '0' {
				token.WriteByte('~')
			} else {
				token.WriteByte('/')
			}
		}
		tokens[n] = token.String()
	}
	for _, token := range tokens {
		switch container := value.(type) {
		case map[string]any:
			var ok bool
			value, ok = container[token]
			if !ok {
				return nil, false, nil
			}
		case []any:
			index, err := strconv.Atoi(token)
			if err != nil || index < 0 || index >= len(container) || token != strconv.Itoa(index) {
				return nil, false, nil
			}
			value = container[index]
		default:
			return nil, false, nil
		}
	}
	return value, true, nil
}

func readScenarios(path string) (*scenarioFile, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var file scenarioFile
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.UseNumber()
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&file); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		return nil, fmt.Errorf("%s: expected one JSON document", path)
	}
	if err := file.validate(); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return &file, nil
}

func (file *scenarioFile) validate() error {
	if file.Version != 1 {
		return fmt.Errorf("unsupported scenario version %d", file.Version)
	}
	if len(file.Cases) == 0 {
		return errors.New("no cases")
	}
	names := map[string]bool{}
	for _, original := range file.Cases {
		c, err := file.resolveCase(original)
		if err != nil {
			return err
		}
		if !caseNamePattern.MatchString(c.Name) || names[c.Name] {
			return fmt.Errorf("invalid or duplicate case name %q", c.Name)
		}
		names[c.Name] = true
		if len(c.Steps) == 0 {
			return fmt.Errorf("%s: no steps", c.Name)
		}
		for _, vars := range []map[string]any{file.Vars, c.Vars} {
			for name := range vars {
				if reservedVariable(name) {
					return fmt.Errorf("%s: reserved variable %q", c.Name, name)
				}
			}
		}
		vars := map[string]any{"project": "project", "run": "run", "case": c.Name}
		for key, value := range file.Vars {
			vars[key] = value
		}
		for key, value := range c.Vars {
			vars[key] = value
		}
		stepNames := map[string]bool{}
		observed := false
		for _, step := range append(slices.Clone(c.Steps), c.Cleanup...) {
			if step.Name == "" || stepNames[step.Name] {
				return fmt.Errorf("%s: empty or duplicate step name %q", c.Name, step.Name)
			}
			stepNames[step.Name] = true
			if (step.REST == nil) == (step.GRPC == nil) {
				return fmt.Errorf("%s/%s: specify exactly one of rest and grpc", c.Name, step.Name)
			}
			if step.REST != nil {
				if step.REST.Method == "" || !strings.HasPrefix(step.REST.Path, "/") || strings.HasPrefix(step.REST.Path, "//") {
					return fmt.Errorf("%s/%s: REST requires a method and an absolute path", c.Name, step.Name)
				}
				if len(step.Until) != 0 && step.REST.Method != "GET" {
					return fmt.Errorf("%s/%s: until requires a REST GET", c.Name, step.Name)
				}
			} else {
				method, err := storageMethod(step.GRPC.Method)
				if err != nil {
					return fmt.Errorf("%s/%s: %w", c.Name, step.Name, err)
				}
				if len(step.Until) != 0 {
					return errors.New("until requires a REST GET")
				}
				if len(step.GRPC.Requests) == 0 || (!method.IsStreamingClient() && len(step.GRPC.Requests) != 1) {
					return fmt.Errorf("%s/%s: invalid request count", c.Name, step.Name)
				}
			}
			for _, request := range []any{step.REST, step.GRPC, step.Until} {
				if err := checkReferences(request, vars, nil); err != nil {
					return fmt.Errorf("%s/%s: %w", c.Name, step.Name, err)
				}
			}
			for name, path := range step.Bind {
				if reservedVariable(name) {
					return fmt.Errorf("%s/%s: cannot bind %q", c.Name, step.Name, name)
				}
				if _, _, err := pointer(nil, path); err != nil {
					return err
				}
			}
			for name := range step.Bind {
				vars[name] = nil
			}
			for _, path := range step.Observe {
				if _, _, err := pointer(nil, path); err != nil {
					return err
				}
			}
			for path := range step.Until {
				if _, _, err := pointer(nil, path); err != nil {
					return err
				}
			}
		}
		for _, step := range c.Steps {
			observed = observed || step.Observe != nil
		}
		if !observed {
			return fmt.Errorf("%s: no observations", c.Name)
		}
		for _, step := range c.Cleanup {
			if step.Observe != nil || len(step.Bind) > 0 {
				return fmt.Errorf("%s: cleanup cannot observe or bind", c.Name)
			}
		}
	}
	return nil
}

func (file *scenarioFile) resolveCase(c scenarioCase) (scenarioCase, error) {
	if c.Template == "" {
		return c, nil
	}
	plan, ok := file.Templates[c.Template]
	if !ok {
		return c, fmt.Errorf("%s: unknown template %q", c.Name, c.Template)
	}
	if c.Steps != nil || c.Cleanup != nil {
		return c, fmt.Errorf("%s: a template case cannot also define steps or cleanup", c.Name)
	}
	c.Steps, c.Cleanup = plan.Steps, plan.Cleanup
	return c, nil
}

// Check scopes without substituting unknown response values. Request payload validation still
// happens after binding; these checks catch misspelled variables before any resources are created.
func checkReferences(value any, vars map[string]any, stack []string) error {
	encoded, err := json.Marshal(value)
	if err != nil {
		return err
	}
	for _, match := range variablePattern.FindAllStringSubmatch(string(encoded), -1) {
		name := match[1]
		value, exists := vars[name]
		if !exists {
			return fmt.Errorf("unknown variable %q", name)
		}
		if slices.Contains(stack, name) {
			return fmt.Errorf("recursive variable %q", name)
		}
		if err := checkReferences(value, vars, append(slices.Clone(stack), name)); err != nil {
			return err
		}
	}
	return nil
}

func reservedVariable(name string) bool { return name == "project" || name == "run" || name == "case" }

func answered(answer json.RawMessage) bool {
	return len(answer) != 0 && !bytes.Equal(bytes.TrimSpace(answer), []byte("null"))
}

func fillScenarios(ctx context.Context, path, project, run string, timeout time.Duration, call scenarioCall) error {
	file, err := readScenarios(path)
	if err != nil {
		return err
	}
	var failures []error
	for i := range file.Cases {
		c := &file.Cases[i]
		if answered(c.Answer) {
			continue
		}
		vars := map[string]any{"project": project, "run": run, "case": c.Name}
		for key, value := range file.Vars {
			vars[key] = value
		}
		for key, value := range c.Vars {
			vars[key] = value
		}
		resolved, err := file.resolveCase(*c)
		if err != nil {
			return err
		}
		fmt.Fprintf(os.Stderr, "%s/%s (project=%s, run=%s)\n", path, c.Name, project, run)
		result, runErr := runScenario(ctx, resolved, vars, timeout, call)
		// A canceled or failed operation leaves the case unanswered, but cleanup still runs with an
		// independent deadline. API rejections of observed operations are answers, not tool failures.
		var cleanupErrors []error
		for _, step := range resolved.Cleanup {
			cleanupCtx, cancel := context.WithTimeout(context.WithoutCancel(ctx), timeout)
			response, err := call(cleanupCtx, step, vars)
			cancel()
			if err == nil && response.Failed {
				missing := false
				if step.REST != nil && step.REST.Method == "DELETE" {
					code, _, _ := pointer(response.Value, "/status")
					missing = code == 404
				}
				if !missing {
					err = fmt.Errorf("cleanup rejected: %v", response.Value)
				}
			}
			if err != nil {
				cleanupErrors = append(cleanupErrors, fmt.Errorf("%s/%s cleanup %s (run=%s): %w", path, c.Name, step.Name, run, err))
			}
		}
		if runErr == nil {
			c.Answer, err = json.Marshal(observation{Project: project, Run: run, Steps: result})
			if err != nil {
				return err
			}
			// Save after each completed case, so a later timeout cannot discard collected answers.
			if err := writeScenarios(path, file); err != nil {
				return err
			}
		} else {
			failures = append(failures, fmt.Errorf("%s/%s: %w", path, c.Name, runErr))
		}
		failures = append(failures, cleanupErrors...)
		if ctx.Err() != nil {
			break
		}
	}
	return errors.Join(failures...)
}

func runScenario(ctx context.Context, c scenarioCase, vars map[string]any, timeout time.Duration, call scenarioCall) (map[string]any, error) {
	results := map[string]any{}
	for _, step := range c.Steps {
		fmt.Fprintf(os.Stderr, "%s/%s\n", c.Name, step.Name)
		stepCtx, cancel := context.WithTimeout(ctx, timeout)
		response, err := pollScenario(stepCtx, step, vars, call)
		cancel()
		if err != nil {
			return nil, fmt.Errorf("%s: %w", step.Name, err)
		}
		if response.Failed {
			if step.Observe == nil {
				return nil, fmt.Errorf("%s: setup request rejected: %v", step.Name, response.Value)
			}
			results[step.Name] = map[string]any{"error": response.Value}
			break
		}
		if step.Observe != nil {
			selected := map[string]any{}
			for name, path := range step.Observe {
				value, found, err := pointer(response.Value, path)
				if err != nil {
					return nil, err
				}
				if found {
					selected[name] = value
				}
			}
			results[step.Name] = selected
		}
		for name, path := range step.Bind {
			value, found, err := pointer(response.Value, path)
			if err != nil {
				return nil, err
			}
			if !found {
				return nil, fmt.Errorf("%s: bind %s: missing %s", step.Name, name, path)
			}
			vars[name] = value
		}
	}
	return results, nil
}

func pollScenario(ctx context.Context, step scenarioStep, vars map[string]any, call scenarioCall) (scenarioResponse, error) {
	untilValue, err := expand(step.Until, vars)
	if err != nil {
		return scenarioResponse{}, err
	}
	until, _ := untilValue.(map[string]any)
	for {
		response, err := call(ctx, step, vars)
		if err != nil || response.Failed {
			return response, err
		}
		done := true
		for path, want := range until {
			got, found, err := pointer(response.Value, path)
			if err != nil {
				return scenarioResponse{}, err
			}
			gotJSON, _ := json.Marshal(got)
			wantJSON, _ := json.Marshal(want)
			done = done && found && bytes.Equal(gotJSON, wantJSON)
		}
		if done {
			return response, nil
		}
		select {
		case <-ctx.Done():
			return scenarioResponse{}, ctx.Err()
		case <-time.After(500 * time.Millisecond):
		}
	}
}

func writeScenarios(path string, file *scenarioFile) error {
	data, err := json.MarshalIndent(file, "", "  ")
	if err != nil {
		return err
	}
	info, err := os.Stat(path)
	if err != nil {
		return err
	}
	temp, err := os.CreateTemp(filepath.Dir(path), ".bqanswers-*")
	if err != nil {
		return err
	}
	defer os.Remove(temp.Name())
	if err := temp.Chmod(info.Mode().Perm()); err != nil {
		temp.Close()
		return err
	}
	if _, err := temp.Write(append(data, '\n')); err != nil {
		temp.Close()
		return err
	}
	if err := temp.Close(); err != nil {
		return err
	}
	return os.Rename(temp.Name(), path)
}
