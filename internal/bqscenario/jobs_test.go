package bqscenario

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func TestSQLJobPollingAndFailures(t *testing.T) {
	for _, mode := range []string{"success", "submission_error", "job_error", "pages", "deadline"} {
		t.Run(mode, func(t *testing.T) {
			polls := 0
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", "application/json")
				switch {
				case r.Method == "POST":
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
			response, err := transport.Call(ctx, Step{SQL: "SELECT 9007199254740993"}, map[string]any{"project": "project"})
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
