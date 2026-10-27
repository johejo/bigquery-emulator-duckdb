package main

import (
	"context"
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
)

func invalidADC(t *testing.T) {
	t.Helper()
	t.Setenv("GOOGLE_APPLICATION_CREDENTIALS", filepath.Join(t.TempDir(), "missing.json"))
}

func TestAPIKeyPrecedesADC(t *testing.T) {
	invalidADC(t)
	u := origin + "/a"
	r := fixture(t, map[string]string{"README.md": u + "\n"})
	serve(t, r, func(w http.ResponseWriter, req *http.Request) {
		if req.Header.Get("X-Goog-Api-Key") != "test-secret" || req.Header.Get("Authorization") != "" {
			t.Error("expected only API key authentication")
		}
		answer(w, u, "# A\n")
	})
	r.client = nil
	if err := r.run(context.Background(), "sync", io.Discard); err != nil {
		t.Fatal(err)
	}
}

func TestOfflineCommandsDoNotLookUpADC(t *testing.T) {
	invalidADC(t)
	r := fixture(t, map[string]string{"README.md": origin + "/a\n"})
	r.client, r.key = nil, ""
	for _, command := range []string{"scan", "check"} {
		err := r.run(context.Background(), command, io.Discard)
		if err != nil && strings.Contains(err.Error(), "authentication") {
			t.Fatalf("%s looked up ADC: %v", command, err)
		}
	}
	if _, err := r.render("docs/example.md", []byte(origin+"/a")); err != nil {
		t.Fatal(err)
	}
	if r.client != nil {
		t.Fatal("offline operations initialized credentials")
	}
	if err := r.run(context.Background(), "sync", io.Discard); err == nil || !strings.Contains(err.Error(), "gcloud auth application-default login") {
		t.Fatalf("expected actionable ADC error, got %v", err)
	}
}

func TestADCRequestsAndQuotaProject(t *testing.T) {
	// A local service-account ADC file exercises the real Google authentication
	// transport and token refresh without user credentials or external requests.
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	var tokenCalls atomic.Int32
	tokenServer := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, req *http.Request) {
		if req.Method != http.MethodPost {
			t.Errorf("token method = %s", req.Method)
		}
		n := tokenCalls.Add(1)
		w.Header().Set("Content-Type", "application/json")
		// Short-lived tokens force refresh on the next document request.
		fmt.Fprintf(w, `{"access_token":"local-token-%d","token_type":"Bearer","expires_in":1}`, n)
	}))
	defer tokenServer.Close()
	credentials := map[string]string{
		"type": "service_account", "project_id": "test-project",
		"private_key_id": "test-key", "client_email": "test@test-project.iam.gserviceaccount.com",
		"private_key": string(pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: mustPKCS8(t, key)})),
		"token_uri":   tokenServer.URL, "quota_project_id": "adc-quota-project",
	}
	b, err := json.Marshal(credentials)
	if err != nil {
		t.Fatal(err)
	}
	file := filepath.Join(t.TempDir(), "adc.json")
	if err := os.WriteFile(file, b, 0600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("GOOGLE_APPLICATION_CREDENTIALS", file)
	for _, quota := range []string{"", "override-quota-project"} {
		t.Run("quota="+quota, func(t *testing.T) {
			t.Setenv("GOOGLE_CLOUD_QUOTA_PROJECT", quota)
			u := origin + "/a"
			r := fixture(t, map[string]string{"README.md": u + "\n"})
			var authorizations []string
			serve(t, r, func(w http.ResponseWriter, req *http.Request) {
				authorizations = append(authorizations, req.Header.Get("Authorization"))
				if !strings.HasPrefix(req.Header.Get("Authorization"), "Bearer local-token-") || req.Header.Get("X-Goog-Api-Key") != "" {
					t.Error("expected only OAuth authentication")
				}
				want := quota
				if want == "" {
					want = "adc-quota-project"
				}
				if got := req.Header.Get("X-Goog-User-Project"); got != want {
					t.Errorf("quota project = %q, want %q", got, want)
				}
				answer(w, u, "# A\n")
			})
			r.client, r.key = nil, ""
			if err := r.run(context.Background(), "sync", io.Discard); err != nil {
				t.Fatal(err)
			}
			if err := r.run(context.Background(), "refresh", io.Discard); err != nil {
				t.Fatal(err)
			}
			if len(authorizations) != 2 || authorizations[0] == authorizations[1] {
				t.Fatalf("token was not refreshed: %v", authorizations)
			}
			// Existing snapshots can be reused even after ADC becomes unavailable.
			t.Setenv("GOOGLE_APPLICATION_CREDENTIALS", filepath.Join(t.TempDir(), "missing.json"))
			r.client = nil
			if err := r.run(context.Background(), "sync", io.Discard); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func mustPKCS8(t *testing.T, key *rsa.PrivateKey) []byte {
	t.Helper()
	b, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		t.Fatal(err)
	}
	return b
}
