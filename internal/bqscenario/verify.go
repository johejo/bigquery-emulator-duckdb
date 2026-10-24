package bqscenario

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"time"
)

func decodeJSON(data []byte, value any) error {
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.UseNumber()
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(value); err != nil {
		return err
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		return errors.New("expected one JSON document")
	}
	return nil
}

func (file *File) variables(c Case, project, run string) map[string]any {
	vars := map[string]any{"project": project, "run": run, "case": c.Name}
	for key, value := range file.Vars {
		vars[key] = value
	}
	for key, value := range c.Vars {
		vars[key] = value
	}
	return vars
}

// Verify executes one case without reading or writing collected answers. Callers
// may run different cases in parallel, using a separate transport for each case.
// Expectations compare the complete selected observation, including missing/null
// distinctions and array order. Answer provenance is never an assertion.
func (file *File) Verify(ctx context.Context, c Case, project, run string, timeout time.Duration, call Call) error {
	if timeout <= 0 {
		return errors.New("scenario timeout must be positive")
	}
	c, err := file.resolveCase(c)
	if err != nil {
		return err
	}
	hasExpect := answered(c.Expect)
	for _, step := range c.Steps {
		hasExpect = hasExpect || step.Expect != nil
	}
	if !hasExpect {
		return fmt.Errorf("%s: missing expect", c.Name)
	}
	vars := file.variables(c, project, run)
	got, runErr, cleanupErr := execute(ctx, c, vars, timeout, call, true)
	if runErr != nil {
		return errors.Join(runErr, cleanupErr)
	}
	if !answered(c.Expect) {
		return cleanupErr
	}
	var want any
	if err := decodeJSON(c.Expect, &want); err != nil {
		return errors.Join(err, cleanupErr)
	}
	want, err = expand(want, vars)
	if err != nil {
		return errors.Join(err, cleanupErr)
	}
	if err := match(got, true, want, c.Name); err != nil {
		return errors.Join(fmt.Errorf("%s: observations differ: %w", c.Name, err), cleanupErr)
	}
	return cleanupErr
}
