#pragma once

#include <string_view>
#include <vector>

#include "src/field_schema.h"
#include "src/references.h"

namespace googlesql {
class TypeFactory;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {
class TableSource;
}  // namespace bigquery_emulator_duckdb

namespace bigquery_emulator_duckdb::information_schema {

// A column of a view and its BigQuery type, from which its GoogleSQL and DuckDB types follow.
struct ViewColumn {
  std::string_view name;
  FieldType type;
  bool repeated = false;
};

// What a view describes: the datasets its qualifier selects.
struct ViewContext {
  TableSource& source;
  googlesql::TypeFactory* type_factory;
  std::vector<DatasetReference> datasets;
};

}  // namespace bigquery_emulator_duckdb::information_schema
