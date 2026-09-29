#!/usr/bin/env bash
# Builds the emulator, starts it on a free port and runs the runn scenarios against it, then
# restarts it on the same --data-dir to check that the data survives.
set -euo pipefail

cd "$(dirname "$0")/../.."

bazelisk build //:bigquery-emulator-duckdb

port="${BQ_EMULATOR_PORT:-$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')}"
export BQ_EMULATOR_API="http://127.0.0.1:${port}"

gcs_port="$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')"
export STORAGE_EMULATOR_HOST="http://127.0.0.1:${gcs_port}"
fake-gcs-server --scheme http --host 127.0.0.1 --port "${gcs_port}" --backend memory --log-level error &
gcs_pid=$!

data_dir="$(mktemp -d)"
emulator_pid=
trap 'kill ${emulator_pid} "${gcs_pid}" 2>/dev/null || true; wait; rm -rf "${data_dir}"' EXIT

start_emulator() {
  ./bazel-bin/bigquery-emulator-duckdb --host 127.0.0.1 --port "${port}" --data-dir "${data_dir}" &
  emulator_pid=$!
  for _ in $(seq 1 50); do
    if curl -fs -o /dev/null "${BQ_EMULATOR_API}/\$discovery/rest?version=v2"; then
      return
    fi
    sleep 0.1
  done
}

for _ in $(seq 1 50); do
  if curl -fs -o /dev/null "${STORAGE_EMULATOR_HOST}/storage/v1/b"; then
    break
  fi
  sleep 0.1
done
curl -fsS -X POST "${STORAGE_EMULATOR_HOST}/storage/v1/b?project=test" \
  -H 'Content-Type: application/json' -d '{"name":"load-fixtures"}' >/dev/null
curl -fsS -X POST "${STORAGE_EMULATOR_HOST}/upload/storage/v1/b/load-fixtures/o?uploadType=media&name=nested%2Fpeople.jsonl" \
  -H 'Content-Type: application/json' --data-binary @tests/e2e/data/people.jsonl >/dev/null

start_emulator

# The exec runner is opt-in. runn also waits for stdin when it is not a TTY, so close it.
runn run --scopes run:exec "$@" tests/e2e/*.yml < /dev/null

# Data written with --data-dir survives a restart, including in a domain-scoped project, whose
# id is escaped into the file name.
tests/e2e/bq.sh mk --dataset persist
tests/e2e/bq.sh query --nouse_legacy_sql "CREATE TABLE persist.t AS SELECT 1 AS a, ['x', 'y'] AS b"
tests/e2e/bq.sh query --nouse_legacy_sql "CREATE VIEW persist.v AS SELECT a, b FROM persist.t"
BQ_EMULATOR_PROJECT=example.com:proj tests/e2e/bq.sh mk --dataset scoped
kill "${emulator_pid}"
wait "${emulator_pid}"
start_emulator
rows="$(tests/e2e/bq.sh query --nouse_legacy_sql --format=json 'SELECT a, b FROM persist.v')"
view="$(tests/e2e/bq.sh show --format=json persist.v)"
datasets="$(BQ_EMULATOR_PROJECT=example.com:proj tests/e2e/bq.sh ls --format=json)"
if [[ "${rows}" != '[{"a":"1","b":["x","y"]}]' || "${view}" != *'"query":"SELECT a, b FROM persist.t"'* || "${datasets}" != *'"datasetId":"scoped"'* ]]; then
  echo "data did not survive a restart: ${rows} ${view} ${datasets}" >&2
  exit 1
fi
