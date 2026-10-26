#!/usr/bin/env bash
# Keep runn's command timing and, when requested, record per-test timings separately.
# Each concurrent invocation must use a distinct report name (the first argument).
set -euo pipefail

report_name=$1
shift
if [[ -z "${E2E_GO_TEST_JSON_DIR:-}" ]]; then
  exec go test "$@"
fi

mkdir -p "$E2E_GO_TEST_JSON_DIR"
# gotestsum prints only failing tests' output, as go test does without -json.
exec gotestsum --format pkgname --hide-summary=skipped --jsonfile "$E2E_GO_TEST_JSON_DIR/$report_name.json" -- "$@"
