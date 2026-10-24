package bqscenario

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func TestSQLJobPollingAndFailures(t *testing.T) {
	for _, mode := range []string{"success", "without_default", "submission_error", "job_error", "pages", "deadline"} {
		t.Run(mode, func(t *testing.T) {
			polls := 0
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", "application/json")
				switch {
				case r.Method == "POST":
					if mode == "without_default" {
						var body struct {
							Configuration struct {
								Query map[string]any `json:"query"`
							} `json:"configuration"`
						}
						if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
							t.Error(err)
						}
						if _, exists := body.Configuration.Query["defaultDataset"]; exists {
							w.WriteHeader(400)
							fmt.Fprint(w, `{"error":{"message":"Invalid default dataset"}}`)
							return
						}
					}
					if mode == "submission_error" {
						w.WriteHeader(400)
						fmt.Fprint(w, `{"error":{"message":"bad submission"}}`)
						return
					}
					fmt.Fprint(w, `{"jobReference":{"jobId":"job"}}`)
				case strings.Contains(r.URL.Path, "/jobs/"):
					polls++
					if mode == "deadline" || polls == 1 {
						fmt.Fprint(w, `{"status":{"state":"RUNNING"}}`)
						return
					}
					if mode == "job_error" {
						fmt.Fprint(w, `{"status":{"state":"DONE","errorResult":{"message":"job failed"}}}`)
						return
					}
					fmt.Fprint(w, `{"status":{"state":"DONE"}}`)
				default:
					if mode == "pages" {
						fmt.Fprint(w, `{"jobComplete":true,"pageToken":"next"}`)
						return
					}
					fmt.Fprint(w, `{"jobComplete":true,"rows":[{"f":[{"v":"9007199254740993"}]}]}`)
				}
			}))
			defer server.Close()
			transport := NewEmulatorTransport(server.URL, "")
			defer transport.Close()
			limit := 2 * time.Second
			if mode == "deadline" {
				limit = 20 * time.Millisecond
			}
			ctx, cancel := context.WithTimeout(t.Context(), limit)
			defer cancel()
			step := Step{SQL: "SELECT 9007199254740993"}
			vars := map[string]any{"project": "project"}
			if mode == "without_default" {
				step.QueryOptions = map[string]any{"defaultDataset": nil}
				vars["dataset"] = "not_created"
			}
			response, err := transport.Call(ctx, step, vars)
			switch mode {
			case "pages", "deadline":
				if err == nil {
					t.Fatalf("%s: expected execution error", mode)
				}
			case "submission_error", "job_error":
				if err != nil || !response.Failed {
					t.Fatalf("rejection: %+v, %v", response, err)
				}
				value, found, _ := pointer(response.Value, "/error/message")
				if !found || value == "" {
					t.Fatalf("lost effective error: %+v", response)
				}
			default:
				if err != nil || response.Failed || polls < 2 {
					t.Fatalf("poll: %+v, %v, polls=%d", response, err, polls)
				}
				value, found, _ := pointer(response.Value, "/result/rows/0/f/0/v")
				if !found || value != "9007199254740993" {
					t.Fatalf("result: %v", response)
				}
			}
		})
	}
}
