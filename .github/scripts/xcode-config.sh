#!/usr/bin/env bash
# Usage: xcode-config.sh DIR
#
# Writes to DIR a repository equivalent to Bazel's local_config_xcode for the selected Xcode only.
# Bazel's own repository runs xcodebuild for every installed Xcode, which takes most of a minute on
# GitHub's macOS runners. Pass DIR to --override_repository for that repository.
set -euo pipefail

dir=$1

out=$(xcodebuild -version)
version=$(sed -n 's/^Xcode //p' <<<"$out")
build=$(sed -n 's/^Build version //p' <<<"$out")
sdk=$(xcrun --sdk macosx --show-sdk-version)
# xcode-locator, which Bazel's repository uses, gives versions three components.
while [[ $(tr -cd . <<<"$version") != .. ]]; do
  version+=.0
done
name="version${version//./_}_$build"

mkdir -p "$dir"
: >"$dir/REPO.bazel"
cat >"$dir/BUILD.bazel" <<BUILD
package(default_visibility = ['//visibility:public'])

xcode_version(
  name = '$name',
  version = '$version.$build',
  aliases = ['$version.$build', '$build', '$version'],
  default_macos_sdk_version = '$sdk',
)
xcode_config(
  name = 'host_xcodes',
  versions = [':$name'],
  default = ':$name',
)
available_xcodes(
  name = 'host_available_xcodes',
  versions = [':$name'],
  default = ':$name',
)
BUILD
