set shell := ["bash", "-euo", "pipefail", "-c"]

_bazel_files := "find . \\( -path './bazel-*' -o -path './external' \\) -prune -o -type f \\( -name 'BUILD' -o -name 'BUILD.bazel' -o -name '*.bzl' -o -name 'MODULE.bazel' \\) -print0"
_cpp_files := "find src tests -type f \\( -name '*.cc' -o -name '*.h' \\) -print0"
_cpp_source_files := "find src tests -type f -name '*.cc' -print0"

fmt:
    {{_bazel_files}} | xargs -0 buildifier
    {{_cpp_files}} | xargs -0 clang-format -i

fmt-check:
    {{_bazel_files}} | xargs -0 buildifier -mode=check
    {{_cpp_files}} | xargs -0 clang-format --dry-run --Werror

refresh-compile-commands:
    bazelisk run //:refresh_compile_commands

tidy: refresh-compile-commands
    {{_cpp_source_files}} | xargs -0 clang-tidy -p .

lint: fmt-check tidy

test:
    bazelisk test //...

check: lint test
