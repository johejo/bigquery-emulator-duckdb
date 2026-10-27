#!/usr/bin/env bash
# Usage: prune-caches.sh KEY PREFIX
#
# Deletes the caches under PREFIX saved before KEY, so that each prefix keeps one entry within the
# repository's quota. Entries saved after KEY, by a later commit's run, are kept.
set -euo pipefail

key=$1
prefix=$2

created=$(gh cache list --key "$key" --limit 100 --json key,createdAt \
  --jq ".[] | select(.key == \"$key\") | .createdAt")
if [[ -z $created ]]; then
  echo "No cache is saved under $key; keeping the others."
  exit 0
fi

gh cache list --key "$prefix" --limit 100 --json id,key,createdAt \
  --jq ".[] | select(.key != \"$key\" and .createdAt < \"$created\") | \"\(.id) \(.key)\"" |
  while read -r id old; do
    echo "Deleting $old"
    gh cache delete "$id"
  done
