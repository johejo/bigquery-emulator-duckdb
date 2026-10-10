#!/usr/bin/env bash
# Usage: run-compliance-shards.sh BUNDLE_DIR LOG_DIR GROUP GROUPS
# Each matrix job runs every GROUPS-th shard, with at most nproc concurrent processes.
set -euo pipefail

bundle=$(realpath "$1")
logs=$(realpath -m "$2")
group=$3
groups=$4
count=$(cat "$bundle/shard-count")
mapfile -d '' -t args < "$bundle/args"
parallelism=$(nproc)
status=0
pids=()

export TEST_SRCDIR="$bundle/compliance_test.runfiles"
export RUNFILES_DIR="$TEST_SRCDIR"
export TEST_WORKSPACE=_main
export TEST_TARGET=//:compliance_test
export BAZEL_TEST=1
export GTEST_TOTAL_SHARDS="$count"
export TEST_TOTAL_SHARDS="$count"
# Use directory-based runfile lookup, never a manifest from the build runner.
unset RUNFILES_MANIFEST_FILE RUNFILES_MANIFEST_ONLY
cd "$TEST_SRCDIR/$TEST_WORKSPACE"

run_shard() {
  local index=$1
  local dir="$logs/shard_$((index + 1))_of_$count"
  mkdir -p "$dir/tmp"
  export TEST_TMPDIR="$dir/tmp"
  export GTEST_SHARD_INDEX="$index"
  export TEST_SHARD_INDEX="$index"
  export GTEST_SHARD_STATUS_FILE="$dir/shard-status"
  export TEST_SHARD_STATUS_FILE="$GTEST_SHARD_STATUS_FILE"
  # Match Bazel's default eternal timeout, and retain partial logs when a shard times out.
  local result=0
  timeout --kill-after=30s 3600 "$bundle/compliance_test" "${args[@]}" > "$dir/test.log" 2>&1 || result=$?
  if [[ ! -f $GTEST_SHARD_STATUS_FILE ]]; then
    echo "Shard $((index + 1)) did not acknowledge sharding" >&2
    result=1
  fi
  return "$result"
}

for ((index = group; index < count; index += groups)); do
  echo "Starting shard $((index + 1)) of $count"
  run_shard "$index" &
  pids+=("$!")
  if ((${#pids[@]} >= parallelism)); then
    wait "${pids[0]}" || status=1
    pids=("${pids[@]:1}")
  fi
done
for pid in "${pids[@]}"; do
  wait "$pid" || status=1
done
exit "$status"
