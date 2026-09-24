#!/usr/bin/env bash
# Builds the emulator, starts it on a free port and runs the runn scenarios against it.
set -euo pipefail

cd "$(dirname "$0")/../.."

bazelisk build //:bigquery-emulator-duckdb

port="${BQ_EMULATOR_PORT:-$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')}"
export BQ_EMULATOR_API="http://127.0.0.1:${port}"

# BQ_EMULATOR_PARSER_FALLBACK=warn|deny reports queries the resolved AST translator misses.
./bazel-bin/bigquery-emulator-duckdb --host 127.0.0.1 --port "${port}" \
  --parser-fallback "${BQ_EMULATOR_PARSER_FALLBACK:-allow}" &
emulator_pid=$!
trap 'kill "${emulator_pid}" 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
  if curl -fs -o /dev/null "${BQ_EMULATOR_API}/\$discovery/rest?version=v2"; then
    break
  fi
  sleep 0.1
done

# The exec runner is opt-in. runn also waits for stdin when it is not a TTY, so close it.
runn run --scopes run:exec "$@" tests/e2e/*.yml < /dev/null
