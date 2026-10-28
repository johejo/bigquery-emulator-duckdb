#pragma once

#include <functional>
#include <memory>
#include <string>

#include "src/statement_resources.h"

namespace googlesql {
class Type;
class Value;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

class TableSource;
struct TemporaryTables;

// The project and dataset that unqualified table and dataset names in DDL belong to, and the
// temporary tables of a multi-statement query that take precedence over them, matching the
// defaults the catalog resolves queries with, plus what the execution backend provides.
// `dataset` may be empty.
struct DefaultDataset {
  std::string project;
  std::string dataset;
  const TemporaryTables* temporary = nullptr;
  // The tables and datasets the statement is analyzed against, such as for the default rounding
  // mode of the dataset that CREATE TABLE AS SELECT creates a table in.
  TableSource* source = nullptr;
  // One statement's source preparation, separate from catalog/schema resolution. Missing for
  // stored definitions, which must not embed staged paths. Expressions borrow their caller's
  // owner through execution; translated statements also retain it with their SQL.
  std::shared_ptr<StatementResources> resources = {};
  // Whether SESSION_USER is available from the execution backend.
  bool has_session_user = false;
  // Whether a query's rows are written to a destination table, which stores them as
  // ResultValue's `stored` gives them.
  bool stores_result = false;
  // Runs a DuckDB query that returns one value of `type`, such as a DDL option expression or
  // a TABLESAMPLE parameter. Without it, values requiring evaluation are unsupported.
  std::function<googlesql::Value(const std::string& query, const googlesql::Type* type)> evaluate =
      {};
};

}  // namespace bigquery_emulator_duckdb
