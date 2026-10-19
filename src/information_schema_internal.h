#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "src/field_schema.h"
#include "src/references.h"

namespace googlesql {
class TypeFactory;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {
class TableSource;
}  // namespace bigquery_emulator_duckdb

// Shared implementation details of the INFORMATION_SCHEMA views, built in the catalog target.
namespace bigquery_emulator_duckdb::information_schema_internal {

// A column of a view and its BigQuery type, from which its GoogleSQL and DuckDB types follow.
struct ViewColumn {
  std::string name;
  FieldType type;
  bool repeated = false;
};

// A cell as a DuckDB literal; an absent value represents SQL NULL.
using Cell = std::optional<std::string>;
using Row = std::vector<Cell>;

inline const Cell kNull;
inline constexpr const char* kEmptyArray = "[]";

// What a view describes: the datasets its qualifier selects.
struct ViewContext {
  TableSource& source;
  googlesql::TypeFactory* type_factory;
  std::vector<DatasetReference> datasets;
};

// An option as SCHEMATA_OPTIONS and TABLE_OPTIONS list it: its name, its type and its value as
// a GoogleSQL literal.
struct Option {
  std::string name;
  std::string type;
  std::string value;
};

FieldSchema ColumnField(const ViewColumn& column);
Cell String(const std::string& value);
Cell Int(int64_t value);
std::string FormatMillis(int64_t millis);
Cell CreationTime(int64_t millis);
Cell OptionalString(const std::string& value);
absl::StatusOr<std::string> ViewSql(const std::vector<ViewColumn>& columns,
                                    const std::vector<Row>& rows);
absl::StatusOr<std::string> TypeName(const FieldSchema& field,
                                     googlesql::TypeFactory* type_factory);
Row Identity(const DatasetReference& dataset);
Row Identity(const DatasetReference& dataset, const std::string& table);
Row Concat(Row identity, const Row& cells);
std::string StringOptionValue(const std::string& value);
std::vector<Option> CommonOptions(const std::string& description, const std::string& friendly_name,
                                  const std::map<std::string, std::string>& labels,
                                  const std::string& default_rounding_mode);
std::string Float64OptionValue(double value);
void AppendOptions(const Row& identity, std::vector<Option> options, std::vector<Row>& rows);

const std::vector<ViewColumn>& SchemataColumns();
absl::StatusOr<std::vector<Row>> SchemataRows(const ViewContext& context);

const std::vector<ViewColumn>& SchemataOptionsColumns();
absl::StatusOr<std::vector<Row>> SchemataOptionsRows(const ViewContext& context);

const std::vector<ViewColumn>& TablesColumns();
absl::StatusOr<std::vector<Row>> TablesRows(const ViewContext& context);

const std::vector<ViewColumn>& TableOptionsColumns();
absl::StatusOr<std::vector<Row>> TableOptionsRows(const ViewContext& context);

const std::vector<ViewColumn>& PartitionsColumns();
absl::StatusOr<std::vector<Row>> PartitionsRows(const ViewContext& context);

const std::vector<ViewColumn>& ViewsColumns();
absl::StatusOr<std::vector<Row>> ViewsRows(const ViewContext& context);

const std::vector<ViewColumn>& ColumnsColumns();
absl::StatusOr<std::vector<Row>> ColumnsRows(const ViewContext& context);

const std::vector<ViewColumn>& ColumnFieldPathsColumns();
absl::StatusOr<std::vector<Row>> ColumnFieldPathsRows(const ViewContext& context);

const std::vector<ViewColumn>& RoutinesColumns();
absl::StatusOr<std::vector<Row>> RoutinesRows(const ViewContext& context);

const std::vector<ViewColumn>& ParametersColumns();
absl::StatusOr<std::vector<Row>> ParametersRows(const ViewContext& context);

}  // namespace bigquery_emulator_duckdb::information_schema_internal
