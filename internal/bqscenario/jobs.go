package bqscenario

import (
	"context"
	"fmt"
	"net/url"
)

// JobRequest uses the native jobs.insert configuration and optional upload media.
// SQL steps build its query configuration; QueryOptions overrides query fields.
type JobRequest struct {
	Configuration any     `json:"configuration"`
	Upload        *string `json:"upload,omitempty"`
	Location      string  `json:"location,omitempty"`
}

func (transport *Transport) job(ctx context.Context, step Step, vars map[string]any) (Response, error) {
	request := step.Job
	if step.SQL != "" {
		query := map[string]any{"query": step.SQL, "useLegacySql": false}
		if dataset, ok := vars["dataset"]; ok {
			query["defaultDataset"] = map[string]any{"projectId": "${project}", "datasetId": dataset}
		}
		for key, value := range step.QueryOptions {
			if value == nil {
				delete(query, key)
			} else {
				query[key] = value
			}
		}
		request = &JobRequest{Configuration: map[string]any{"query": query}}
	}
	expanded, err := expandedRequest(*request, vars)
	if err != nil {
		return Response{}, err
	}
	location := expanded.Location
	if location == "" {
		location = "US"
	}
	project, ok := vars["project"].(string)
	if !ok {
		return Response{}, fmt.Errorf("job requires project")
	}
	base := "/bigquery/v2/projects/" + url.PathEscape(project)
	path := base + "/jobs"
	if expanded.Upload != nil {
		path = "/upload" + path + "?uploadType=multipart"
	}
	inserted, err := transport.rest(ctx, RESTRequest{Method: "POST", Path: path, Body: map[string]any{"jobReference": map[string]any{"location": location}, "configuration": expanded.Configuration}, Upload: expanded.Upload}, nil)
	if err != nil {
		return Response{}, err
	}
	if inserted.Failed {
		return jobError(inserted), nil
	}
	id, found, err := pointer(inserted.Value, "/body/jobReference/jobId")
	if err != nil || !found {
		return Response{}, fmt.Errorf("jobs.insert: missing job ID")
	}
	jobPath := base + "/jobs/" + url.PathEscape(fmt.Sprint(id)) + "?location=" + url.QueryEscape(location)
	result, err := pollScenario(ctx, Step{REST: &RESTRequest{Method: "GET", Path: jobPath}, Until: map[string]any{"/body/status/state": "DONE"}}, nil, transport.Call)
	if err != nil {
		return Response{}, err
	}
	if result.Failed {
		return jobError(result), nil
	}
	if failure, found, _ := pointer(result.Value, "/body/status/errorResult"); found && failure != nil {
		result.Failed = true
		result.Value.(map[string]any)["error"] = failure
		return result, nil
	}
	config, _ := expanded.Configuration.(map[string]any)
	if _, query := config["query"]; query {
		read, err := pollScenario(ctx, Step{REST: &RESTRequest{Method: "GET", Path: base + "/queries/" + url.PathEscape(fmt.Sprint(id)) + "?location=" + url.QueryEscape(location) + "&maxResults=10000"}, Until: map[string]any{"/body/jobComplete": true}}, nil, transport.Call)
		if err != nil {
			return Response{}, err
		}
		if read.Failed {
			return jobError(read), nil
		}
		result.Value.(map[string]any)["result"] = read.Value.(map[string]any)["body"]
		if token, found, _ := pointer(read.Value, "/body/pageToken"); found && token != "" {
			return Response{}, fmt.Errorf("SQL step result is paginated; use explicit REST steps")
		}
	}
	return result, nil
}

func jobError(response Response) Response {
	if value, found, _ := pointer(response.Value, "/body/error"); found {
		response.Value.(map[string]any)["error"] = value
	}
	return response
}
