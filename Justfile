set shell := ["bash", "-euo", "pipefail", "-c"]

# Tracked and untracked files, minus those that .gitignore and .git/info/exclude ignore, such as
# Bazel output symlinks and worktrees under .claude.
_git_files := "git ls-files -z --cached --others --exclude-standard --"
_bazel_files := _git_files + " ':(glob)**/BUILD' ':(glob)**/BUILD.bazel' ':(glob)**/*.bzl' ':(glob)**/MODULE.bazel'"
_cpp_files := _git_files + " ':(glob)src/**/*.cc' ':(glob)src/**/*.h' ':(glob)tests/**/*.cc' ':(glob)tests/**/*.h' ':(glob)tools/**/*.cc' ':(glob)tools/**/*.h'"

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

# The DuckDB CLI in the dev shell and the libraries linked into the binary come from the same
# release.
duckdb-version-check:
    nix_version=$(sed -n 's/^ *version = "\(.*\)";$/\1/p' duckdb-bin.nix); \
    bazel_versions=$(grep -o 'duckdb/releases/download/v[^/]*\|install\.duckdb\.org/v[^/]*' MODULE.bazel | sed 's/.*\/v//' | sort -u); \
    if [[ "$bazel_versions" != "$nix_version" ]]; then \
        echo "DuckDB versions differ: duckdb-bin.nix has $nix_version, MODULE.bazel has $(echo $bazel_versions)" >&2; \
        exit 1; \
    fi

lint: fmt-check duckdb-version-check tidy

test:
    bazelisk test //...

# End-to-end tests: start the emulator and drive it with the bq command-line tool via runn.
e2e *args:
    tests/e2e/run.sh {{args}}

run *args:
    bazelisk run //:bigquery-emulator-duckdb -- {{args}}

check: lint test e2e

# Regenerates docs/functions.md, docs/sql.md and docs/api.md, the tables of which BigQuery
# functions, SQL features and REST API methods the emulator supports.
docs:
    bazelisk build //:functions_md //:sql_md //:api_md
    install -m 644 bazel-bin/functions.md docs/functions.md
    install -m 644 bazel-bin/sql.md docs/sql.md
    install -m 644 bazel-bin/api.md docs/api.md
