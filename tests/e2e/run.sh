#!/usr/bin/env bash
# Builds the emulator and runs the runn scenarios against it and a fake GCS server preloaded with
# tests/e2e/gcs, then restarts the emulator on the same --data-dir to check that data survives.
set -euo pipefail

cd "$(dirname "$0")/../.."

bazelisk build //:bigquery-emulator-duckdb

free_port() {
  python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'
}

wait_for() {
  for _ in $(seq 1 50); do
    curl -fs -o /dev/null "$1" && return
    sleep 0.1
  done
  echo "timed out waiting for $1" >&2
  return 1
}

# runn waits for stdin when it is not a TTY, so close it. The exec runner is opt-in.
runn_run() {
  runn run --scopes run:exec "$@" < /dev/null
}

tmp="$(mktemp -d)"
pids=()
trap 'kill "${pids[@]}" 2>/dev/null || true; wait; rm -rf "${tmp}"' EXIT
# Where scenarios keep files they download.
export E2E_TMP_DIR="${tmp}/files"
mkdir "${E2E_TMP_DIR}" "${tmp}/data"

port="${BQ_EMULATOR_PORT:-$(free_port)}"
export BQ_EMULATOR_API="http://127.0.0.1:${port}"
gcs_port="$(free_port)"
export STORAGE_EMULATOR_HOST="http://127.0.0.1:${gcs_port}"

# Each directory under tests/e2e/gcs becomes a bucket holding the files beneath it.
fake-gcs-server --scheme http --host 127.0.0.1 --port "${gcs_port}" --backend memory \
  --data tests/e2e/gcs --log-level error &
pids+=($!)
wait_for "${STORAGE_EMULATOR_HOST}/storage/v1/b"

start_emulator() {
  ./bazel-bin/bigquery-emulator-duckdb --host 127.0.0.1 --port "${port}" --data-dir "${tmp}/data" &
  emulator_pid=$!
  pids+=("${emulator_pid}")
  wait_for "${BQ_EMULATOR_API}/\$discovery/rest?version=v2"
}

start_emulator
# Runbooks run concurrently, each on datasets of its own.
runn_run --concurrent on "$@" tests/e2e/*.yml
runn_run tests/e2e/restart/before.yml
kill "${emulator_pid}"
wait "${emulator_pid}"
start_emulator
runn_run tests/e2e/restart/after.yml
