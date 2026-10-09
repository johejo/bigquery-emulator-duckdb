set shell := ["bash", "-euo", "pipefail", "-c"]

# Formatters and the files they cover are in treefmt.toml.
fmt:
    treefmt

fmt-check:
    treefmt --ci

# compile_commands.json is for clangd; clang-tidy no longer needs it.
refresh-compile-commands:
    bazelisk run //:refresh_compile_commands

tidy:
    bazelisk build --config=clang-tidy //...

# Supplement clang-tidy with local checks. Dependency headers are deliberately
# omitted: Cppcheck cannot reliably parse all of them, so whole-program unused
# function checks would produce false positives.
cppcheck *args:
    mkdir -p .cache/cppcheck
    cppcheck --quiet -j "$(nproc)" --cppcheck-build-dir=.cache/cppcheck \
        --std=c++20 -I . --enable=warning,style,performance,portability \
        --library=googletest --inline-suppr --error-exitcode=1 \
        --suppressions-list=.cppcheck-suppressions {{args}} src tests tools

# The DuckDB CLI in the dev shell and the libraries linked into the binary come from the same
# release.
duckdb-version-check:
    nix_version=$(sed -n 's/^ *version = "\(.*\)";$/\1/p' duckdb-bin.nix); \
    bazel_versions=$(grep -o 'duckdb/releases/download/v[^/]*\|install\.duckdb\.org/v[^/]*' MODULE.bazel | sed 's/.*\/v//' | sort -u); \
    if [[ "$bazel_versions" != "$nix_version" ]]; then \
        echo "DuckDB versions differ: duckdb-bin.nix has $nix_version, MODULE.bazel has $(echo $bazel_versions)" >&2; \
        exit 1; \
    fi

go-vet:
    go vet ./...

# Tool and subprocess tests; client-visible tests run through just e2e.
[positional-arguments]
go-test *args:
    go test "$@" ./internal/... ./tools/...

go-tidy:
    go mod tidy

lint: fmt-check duckdb-version-check go-vet cppcheck tidy

test: go-test
    bazelisk test //...

# Build the emulator, then manage E2E servers and run the runn CLI scenarios.
# Extra arguments go to the main runn invocation.
[positional-arguments]
e2e *args:
    bazelisk build //:bigquery-emulator-duckdb
    go run ./tools/e2e "$@"

# GoogleSQL's compliance tests against the emulator; not part of `check`, since they take long.
# Extra arguments go to `bazelisk test`, such as --test_arg=--gtest_filter=...
compliance *args:
    bazelisk test //:compliance_test {{args}}

# Summarizes the last `just compliance` run as Markdown, and fails when a shard did not finish or a
# known failure passes.
# Extra arguments go to tools/compliancesummary, such as -results FILE for each statement's outcome.
compliance-summary *args:
    go run ./tools/compliancesummary {{args}} bazel-testlogs/compliance_test

# Evaluates a query on GoogleSQL's reference implementation, which is not BigQuery: a lead for
# what to check on BigQuery, never an expected value.
reference sql:
    bazelisk run @googlesql//googlesql/tools/execute_query -- --product_mode=external {{quote(sql)}}

# Writes BigQuery's answers into tests/e2e/goclient/testdata/unverified.txt; for maintainers only,
# since queries on BigQuery are billed.
bigquery-answers project:
    go run ./tools/bqanswers {{quote(project)}} {{justfile_directory()}}/tests/e2e/goclient/testdata/unverified.txt

[positional-arguments]
run *args:
    bazelisk run //:bigquery-emulator-duckdb -- "$@"

check: lint test e2e docs-check

# Generates docs/sql.md and docs/api.md: tools/restprobe probes emulators started from the binary.
_restprobe := "go run ./tools/restprobe -emulator " + justfile_directory() + "/bazel-bin/bigquery-emulator-duckdb"
_sql_probe := _restprobe + " sql " + justfile_directory() + "/tools/sql_features.txt"
_api_probe := _restprobe + " api " + justfile_directory() + "/third_party/bigquery/discovery.json " + justfile_directory() + "/tools/api_methods.txt"

# Regenerates docs/functions.md, docs/sql.md and docs/api.md, the tables of which BigQuery
# functions, SQL features and REST API methods the emulator supports.
docs:
    bazelisk build //:functions_md //:bigquery-emulator-duckdb
    install -m 644 bazel-bin/functions.md docs/functions.md
    {{_sql_probe}} > docs/sql.md.tmp && mv docs/sql.md.tmp docs/sql.md
    {{_api_probe}} > docs/api.md.tmp && mv docs/api.md.tmp docs/api.md

# Fails when docs/sql.md or docs/api.md is stale; `just test` checks docs/functions.md.
docs-check:
    bazelisk build //:bigquery-emulator-duckdb
    {{_sql_probe}} | diff -u docs/sql.md - || { echo "docs/sql.md is stale; run just docs" >&2; exit 1; }
    {{_api_probe}} | diff -u docs/api.md - || { echo "docs/api.md is stale; run just docs" >&2; exit 1; }
