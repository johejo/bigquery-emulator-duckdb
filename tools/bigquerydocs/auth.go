package main

import (
	"context"
	"fmt"
	"net/http"
	"time"

	"google.golang.org/api/option"
	httptransport "google.golang.org/api/transport/http"
)

// initClient is lazy: inspecting snapshots and reusing existing pages never needs
// credentials. An explicit API key takes precedence over ADC, including broken ADC.
func (r *repo) initClient(ctx context.Context) error {
	if r.client != nil {
		return nil
	}
	client := &http.Client{}
	if r.key == "" {
		var err error
		client, _, err = httptransport.NewClient(ctx,
			option.WithScopes("https://www.googleapis.com/auth/cloud-platform"))
		if err != nil {
			return fmt.Errorf("initialize documentation authentication: configure ADC with gcloud auth application-default login, or set DEVELOPERKNOWLEDGE_API_KEY: %w", err)
		}
	}
	client.Timeout = 45 * time.Second
	// Do not forward credentials through a redirect or silently fetch another resource.
	client.CheckRedirect = func(_ *http.Request, _ []*http.Request) error {
		return http.ErrUseLastResponse
	}
	r.client = client
	return nil
}
