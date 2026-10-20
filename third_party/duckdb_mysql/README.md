# duckdb_mysql

DuckDB's [mysql_scanner](https://github.com/duckdb/duckdb-mysql) extension is compiled against
`duckdb_source` and linked statically, as [postgres_scanner](../duckdb_postgres/README.md) is.
`MODULE.bazel` pins the commit that DuckDB v1.5.6 uses. Both extensions use the same
`database-connector` sources.

`bigquery_types.patch` reads DECIMAL as VARCHAR so no digits are lost before EXTERNAL_QUERY
applies its decimal option. It also identifies binary values by character set 63, since MySQL's
BINARY_FLAG marks text with binary collation too. Zero dates are rejected rather than silently
converted to NULL; their BigQuery behavior remains in the unverified cases. `static_symbols.patch` gives two helpers
internal linkage to avoid collisions with postgres_scanner.

[src/mysql_query.cc](../../src/mysql_query.cc) registers the extension, disables TINYINT(1)
boolean inference and sets MySQL sessions to UTC. MariaDB Connector/C uses the extension's
connection parser to describe query result fields before execution.
