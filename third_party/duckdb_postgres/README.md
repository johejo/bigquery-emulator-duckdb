# duckdb_postgres

DuckDB's [postgres_scanner](https://github.com/duckdb/duckdb-postgres) extension, built from source
and linked statically into the binary, since the prebuilt libduckdb in `third_party/duckdb` does not
include it. Loading the extension at runtime would need the executable to export DuckDB's symbols,
which it deliberately does not (see the `bigquery-emulator-duckdb` target in `BUILD.bazel`).

`MODULE.bazel` pins the extension and its database-connector submodule to the commits that the
DuckDB release in use pins. The extension compiles against `BUILD.duckdb_source.bazel`, the C++
headers of that release's source; nothing else from the source is built, so the DuckDB version in
the `duckdb_source` archive must match the prebuilt libraries'. libpq comes from the `postgres`
module, built against BoringSSL (see `.bazelrc`).

`src/postgres_scanner.cc` registers the extension on a database opened through the C API.
