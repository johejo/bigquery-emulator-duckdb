#!/usr/bin/env bash
# Build has already produced compliance_test. Package its runfiles and resolved arguments so
# matrix jobs can run the same test without rebuilding or duplicating BUILD.bazel's flags.
# Usage: package-compliance.sh OUTPUT_DIR
set -euo pipefail

output=$(realpath -m "$1")
mkdir -p "$output/summary"
bazelisk aquery 'mnemonic("TestRunner", //:compliance_test)' \
  --output=jsonproto --include_artifacts=false > "$output/actions.json"
python3 - "$output" <<'PY'
import json
import pathlib
import sys

output = pathlib.Path(sys.argv[1])
actions = json.loads((output / "actions.json").read_text())["actions"]
args = actions[0]["arguments"]
# Fail if Bazel's launcher changes; never silently omit a new wrapper or different shard flags.
assert args[:2] == ["external/bazel_tools/tools/test/test-setup.sh", "./compliance_test"]
assert all(action["arguments"] == args for action in actions)
(output / "args").write_bytes(b"".join(arg.encode() + b"\0" for arg in args[2:]))
(output / "summary" / "shard-count").write_text(str(len(actions)) + "\n")
PY
# Dereference Bazel's symlinks so the archive has no dependency on the build runner's paths.
# The manifest contains absolute paths; consumers use the runfiles directory instead.
tar --dereference --exclude=compliance_test.runfiles/MANIFEST \
  -czf "$output/compliance.tar.gz" -C bazel-bin compliance_test compliance_test.runfiles \
  -C "$output" args -C "$output/summary" shard-count
go build -o "$output/summary/compliance-summary" ./tools/compliancesummary
