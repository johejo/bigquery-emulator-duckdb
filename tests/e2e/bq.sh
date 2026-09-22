#!/usr/bin/env bash
# Runs the bq command-line tool against the emulator with authentication disabled.
#
# bq normally obtains credentials through gcloud; --oauth_access_token with a dummy value and
# --nouse_google_auth skip that entirely, which is enough because the emulator ignores auth.
set -euo pipefail

: "${BQ_EMULATOR_API:=http://127.0.0.1:9050}"
: "${BQ_EMULATOR_PROJECT:=test}"
: "${BQ_EMULATOR_STATE_DIR:=${TMPDIR:-/tmp}/bigquery-emulator-duckdb-e2e}"

# Keep bq and gcloud from reading or writing the user's real configuration.
mkdir -p "${BQ_EMULATOR_STATE_DIR}/gcloud"
: > "${BQ_EMULATOR_STATE_DIR}/bigqueryrc"
export CLOUDSDK_CONFIG="${BQ_EMULATOR_STATE_DIR}/gcloud"
export BIGQUERYRC="${BQ_EMULATOR_STATE_DIR}/bigqueryrc"
export CLOUDSDK_CORE_DISABLE_USAGE_REPORTING=true

exec bq \
  --api "${BQ_EMULATOR_API}" \
  --project_id "${BQ_EMULATOR_PROJECT}" \
  --oauth_access_token=dummy \
  --nouse_google_auth \
  --headless \
  "$@"
