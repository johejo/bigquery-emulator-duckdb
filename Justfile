set shell := ["bash", "-euo", "pipefail", "-c"]

# Nix apps supply test dependencies separately from the build tools. Outside Nix, use PATH.
_test_env := 'env PATH="${TEST_TOOLS_PATH:+$TEST_TOOLS_PATH:}$PATH"'

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

heavy-includes:
    bazelisk build --config=heavy-includes //...

# Supplement clang-tidy with local checks. Dependency headers are deliberately
# omitted: Cppcheck cannot reliably parse all of them, so whole-program unused
# function checks would produce false positives.
cppcheck *args:
    mkdir -p .cache/cppcheck
    cppcheck --quiet -j "$(nproc)" --cppcheck-build-dir=.cache/cppcheck \
        --std=c++20 -I . --enable=warning,style,performance,portability \
        --library=googletest --inline-suppr --error-exitcode=1 \
        --suppressions-list=.cppcheck-suppressions {{ args }} src tests tools

# The DuckDB CLI in the dev shell, the libraries linked into the binary and the headers the
# postgres_scanner extension compiles against come from the same release.
duckdb-version-check:
    nix_version=$(sed -n 's/^ *version = "\(.*\)";$/\1/p' duckdb-bin.nix); \
    bazel_versions=$(grep -o 'duckdb/releases/download/v[^/]*\|install\.duckdb\.org/v[^/]*\|duckdb/duckdb/archive/refs/tags/v[0-9.]*[0-9]' MODULE.bazel | sed 's/.*\/v//' | sort -u); \
    if [[ "$bazel_versions" != "$nix_version" ]]; then \
        echo "DuckDB versions differ: duckdb-bin.nix has $nix_version, MODULE.bazel has $(echo $bazel_versions)" >&2; \
        exit 1; \
    fi

go-vet:
    go vet ./...

go-staticcheck:
    staticcheck ./...

# Tool and subprocess tests; client-visible tests run through just e2e. Set GO_TEST_JSON to a
# path to also record go test's JSON events there, such as each test's duration.
[positional-arguments]
go-test *args:
    if [[ -n "${GO_TEST_JSON:-}" ]]; then \
        {{ _test_env }} gotestsum --format pkgname --hide-summary=skipped --jsonfile "$GO_TEST_JSON" -- "$@" ./internal/... ./tools/...; \
    else \
        {{ _test_env }} go test "$@" ./internal/... ./tools/...; \
    fi

go-tidy:
    go mod tidy

lint: fmt-check duckdb-version-check go-vet go-staticcheck cppcheck tidy heavy-includes

test: go-test
    bazelisk test //...

# Build the emulator, then manage E2E servers and run the runn CLI scenarios.
# Extra arguments go to the main runn invocation.
[positional-arguments]
e2e *args:
    bazelisk build //:bigquery-emulator-duckdb
    {{ _test_env }} go run ./tools/e2e "$@"

# GoogleSQL's compliance tests against the emulator; not part of `check`, since they take long.
# Extra arguments go to `bazelisk test`, such as --test_arg=--gtest_filter=...
compliance *args:
    bazelisk test //:compliance_test {{ args }}

# Summarizes the last `just compliance` run as Markdown, and fails when a shard did not finish or a
# known failure passes.
# Extra arguments go to tools/compliancesummary, such as -results FILE for each statement's outcome.
compliance-summary *args:
    go run ./tools/compliancesummary {{ args }} bazel-testlogs/compliance_test

# Evaluates a query on GoogleSQL's reference implementation, which is not BigQuery: a lead for
# what to check on BigQuery, never an expected value.
reference sql:
    bazelisk run @googlesql//googlesql/tools/execute_query -- --product_mode=external {{ quote(sql) }}

# Collects SQL, REST and gRPC answers on BigQuery; for maintainers only, since requests can be billed.
bigquery-answers project path=(justfile_directory() + "/tests/e2e/goclient/testdata/unverified"):
    go run ./tools/bqanswers {{ quote(project) }} {{ quote(path) }}

# Checks API case files without authentication, requests or modifying answers.
bigquery-answers-validate path=(justfile_directory() + "/tests/e2e/goclient/testdata/unverified"):
    go run ./tools/bqanswers -validate unused {{ quote(path) }}

[positional-arguments]
run *args:
    bazelisk run //:bigquery-emulator-duckdb -- "$@"

check: lint test e2e docs-check

# Generates SQL, DDL option, REST and function reports: tools/restprobe probes emulators started
# from the binary, running the calls //:function_probe builds for functions.
_restprobe := _test_env + " go run ./tools/restprobe -emulator " + justfile_directory() + "/bazel-bin/bigquery-emulator-duckdb"
_sql_probe := _restprobe + " sql " + justfile_directory() + "/tools/sql_features.txt"
_api_probe := _restprobe + " api " + justfile_directory() + "/third_party/bigquery/discovery.json " + justfile_directory() + "/tools/api_methods.txt"
_function_probe := _restprobe + " functions " + justfile_directory() + "/bazel-bin/function_probe " + justfile_directory() + "/tools/bigquery_functions.txt " + justfile_directory() + "/tools/function_probe_hints.txt"
_option_probe := _restprobe + " options " + justfile_directory() + "/tools/ddl_options.json"
_storage_probe := _test_env + " go run ./tools/storageprobe -emulator " + justfile_directory() + "/bazel-bin/bigquery-emulator-duckdb"

# Regenerates the compatibility tables for functions, SQL, DDL options, REST and Storage RPCs.
docs:
    bazelisk build //:function_probe //:bigquery-emulator-duckdb
    {{ _function_probe }} > docs/functions.md.tmp && mv docs/functions.md.tmp docs/functions.md
    {{ _sql_probe }} > docs/sql.md.tmp && mv docs/sql.md.tmp docs/sql.md
    {{ _option_probe }} > docs/options.md.tmp && mv docs/options.md.tmp docs/options.md
    {{ _api_probe }} > docs/api.md.tmp && mv docs/api.md.tmp docs/api.md
    {{ _storage_probe }} > docs/storage.md.tmp && mv docs/storage.md.tmp docs/storage.md

# Fails when function, SQL, DDL option, REST or Storage docs are stale.
docs-check:
    bazelisk build //:function_probe //:bigquery-emulator-duckdb
    {{ _function_probe }} | diff -u docs/functions.md - || { echo "docs/functions.md is stale; run just docs" >&2; exit 1; }
    {{ _sql_probe }} | diff -u docs/sql.md - || { echo "docs/sql.md is stale; run just docs" >&2; exit 1; }
    {{ _option_probe }} | diff -u docs/options.md - || { echo "docs/options.md is stale; run just docs" >&2; exit 1; }
    {{ _api_probe }} | diff -u docs/api.md - || { echo "docs/api.md is stale; run just docs" >&2; exit 1; }
    {{ _storage_probe }} | diff -u docs/storage.md - || { echo "docs/storage.md is stale; run just docs" >&2; exit 1; }

# Regenerates docs/options.md alone, while editing tools/ddl_options.json.
docs-options:
    bazelisk build //:bigquery-emulator-duckdb
    {{ _option_probe }} > docs/options.md.tmp && mv docs/options.md.tmp docs/options.md

# Refreshes the option names and types in tools/ddl_options.json from BigQuery's DDL reference.
update-ddl-options:
    curl -fsSL https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/data-definition-language | go run ./tools/ddloptions tools/ddl_options.json
