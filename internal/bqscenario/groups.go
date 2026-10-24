package bqscenario

import (
	"encoding/json"
	"fmt"
	"strings"
)

// Groups are flat sequences, expanded before validation and execution. Prefixing
// their step names and bindings keeps repeated calls independent without adding
// another runtime scope or changing collection and failure handling.
func (file *File) expandGroups(steps []Step) ([]Step, error) {
	var result []Step
	for _, call := range steps {
		if call.Group == "" {
			result = append(result, call)
			continue
		}
		members, ok := file.StepGroups[call.Group]
		if !caseNamePattern.MatchString(call.Name) {
			return nil, fmt.Errorf("invalid group call name %q", call.Name)
		}
		if !ok || len(members) == 0 {
			return nil, fmt.Errorf("%s: unknown or empty step group %q", call.Name, call.Group)
		}
		// A group call supplies only its name and scoped arguments.
		hasRequest := call.REST != nil || call.GRPC != nil || call.Job != nil || call.SQL != ""
		hasOverrides := call.Observe != nil || call.Bind != nil || call.Expect != nil || call.ExpectFailure
		if hasRequest || hasOverrides || call.Use != "" || call.QueryOptions != nil || call.Until != nil {
			return nil, fmt.Errorf("%s: group call allows only name, group and vars", call.Name)
		}
		for _, member := range members {
			if member.Group != "" {
				return nil, fmt.Errorf("%s: nested step groups are unsupported", call.Name)
			}
		}
		members, err := file.resolveSteps(members)
		if err != nil {
			return nil, err
		}
		replacements := []string{}
		for _, member := range members {
			for binding := range member.Bind {
				if reservedVariable(binding) {
					return nil, fmt.Errorf("%s: cannot bind %q", call.Name, binding)
				}
				replacements = append(replacements, "${"+binding+"}", "${"+call.Name+"_"+binding+"}")
			}
		}
		rename := strings.NewReplacer(replacements...)
		for _, member := range members {
			if member.Name == "" {
				return nil, fmt.Errorf("%s: group member requires a name", call.Name)
			}
			data, err := json.Marshal(member)
			if err != nil {
				return nil, err
			}
			var expanded Step
			if err := decodeJSON([]byte(rename.Replace(string(data))), &expanded); err != nil {
				return nil, err
			}
			// An empty selection is meaningful for expected rejections, but is
			// omitted by Step's JSON encoding.
			if member.Observe != nil && expanded.Observe == nil {
				expanded.Observe = map[string]string{}
			}
			expanded.Name = call.Name + "_" + member.Name
			expanded.Bind = nil
			for binding, path := range member.Bind {
				if expanded.Bind == nil {
					expanded.Bind = map[string]string{}
				}
				expanded.Bind[call.Name+"_"+binding] = path
			}
			vars := map[string]any{}
			for key, value := range call.Vars {
				vars[key] = value
			}
			for key, value := range expanded.Vars {
				vars[key] = value
			}
			expanded.Vars = vars
			result = append(result, expanded)
		}
	}
	return result, nil
}
