#!/usr/bin/env bash
# Run through the Justfile from the repository root. No builds or service credentials needed.
set -euo pipefail

mode=${1:-report}
if [[ "$mode" != report && "$mode" != check ]]; then
  echo "usage: $0 {report|check} [//:target ...]" >&2
  exit 2
fi
if [[ $# -gt 0 ]]; then shift; fi
if [[ $# -eq 0 ]]; then set -- //:compliance_test; fi

# These existing targets represent application composition, acquisition and extraction.
# Include the SDK itself so a new path bypassing :gcs is also caught.
forbidden='set(//:emulator //:gcs //:file_read //:extract @google_cloud_cpp//:storage)'
failed=0
for root in "$@"; do
  # Resolve separately: a misspelled or missing root must never count as an empty path.
  bazelisk query "$root" --output=label >/dev/null
  if [[ "$mode" == report ]]; then
    for sink in //:gcs @google_cloud_cpp//:storage @google_cloud_cpp//:spanner; do
      printf '\n# All dependency paths: %s -> %s\n' "$root" "$sink"
      bazelisk query "allpaths($root, $sink)" --output=graph --nograph:factored
    done
  else
    paths=$(bazelisk query "allpaths($root, $forbidden)" --output=label)
    if [[ -n "$paths" ]]; then
      printf 'Forbidden runtime dependency reachable from %s:\n%s\n' "$root" "$paths" >&2
      failed=1
    else
      printf 'No application/Storage dependency reachable from %s\n' "$root"
    fi
  fi
done
exit "$failed"
