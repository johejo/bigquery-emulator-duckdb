set shell := ["bash", "-euo", "pipefail", "-c"]

_bazel_files := "find . \\( -path './bazel-*' -o -path './external' \\) -prune -o -type f \\( -name 'BUILD' -o -name 'BUILD.bazel' -o -name '*.bzl' -o -name 'MODULE.bazel' \\) -print0"
_cpp_files := "find src tests -type f \\( -name '*.cc' -o -name '*.h' \\) -print0"

fmt:
    {{_bazel_files}} | xargs -0 buildifier
    {{_cpp_files}} | xargs -0 clang-format -i

fmt-check:
    {{_bazel_files}} | xargs -0 buildifier -mode=check
    {{_cpp_files}} | xargs -0 clang-format --dry-run --Werror

# compile_commands.json is for clangd; clang-tidy no longer needs it.
refresh-compile-commands:
    bazelisk run //:refresh_compile_commands

tidy:
    bazelisk build --config=clang-tidy //...

lint: fmt-check tidy

test:
    bazelisk test //...

# End-to-end tests: start the emulator and drive it with the bq command-line tool via runn.
e2e *args:
    tests/e2e/run.sh {{args}}

run *args:
    bazelisk run //:bigquery-emulator-duckdb -- {{args}}

check: lint test e2e
