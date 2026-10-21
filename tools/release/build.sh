#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

: "${RELEASE_GLIBC:?Run nix run .#release to select the glibc release toolchain}"
: "${FHS_INTERPRETER:?Missing FHS interpreter for this architecture}"

bazel=(bazelisk "--output_base=$PWD/.cache/release/bazel")
"${bazel[@]}" build --config=release "$@" //:bigquery-emulator-duckdb
binary_dir=$("${bazel[@]}" info --config=release bazel-bin)
output="$PWD/.cache/release/bigquery-emulator-duckdb"
install -m755 "$binary_dir/bigquery-emulator-duckdb" "$output"
patchelf --set-interpreter "$FHS_INTERPRETER" --remove-rpath "$output"
python3 tools/release/check.py "$output" "$FHS_INTERPRETER"

# NixOS has no FHS interpreter. Run with the old loader explicitly; do not let
# LD_LIBRARY_PATH substitute newer libraries for the baseline being checked.
env -u LD_LIBRARY_PATH "$RELEASE_GLIBC/lib/$(basename "$FHS_INTERPRETER")" \
  --library-path "$RELEASE_GLIBC/lib" "$output" shell \
  --project='{"projectId":"release-check"}' -e 'SELECT 1 AS x'
echo "$output"
