#include "src/catalog_table.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/table_metadata.h"
#include "src/table_source.h"

namespace bigquery_emulator_duckdb {

std::optional<TableDescription> BigQueryTable::Describe() const {
  return source_.DescribeTable(path_.at(0), path_.at(1), path_.at(2));
}

std::optional<std::vector<int>> BigQueryTable::PrimaryKey() const {
  const std::optional<TableDescription> description = Describe();
  if (!description || description->metadata.primary_key.empty()) {
    return std::nullopt;
  }
  std::vector<int> columns;
  for (const std::string& name : description->metadata.primary_key) {
    const auto column = std::ranges::find_if(schema_, [&](const FieldSchema& candidate) {
      return ToLowerAscii(candidate.name) == ToLowerAscii(name);
    });
    if (column == schema_.end()) {
      return std::nullopt;
    }
    columns.push_back(static_cast<int>(column - schema_.begin()));
  }
  return columns;
}

std::optional<std::string> BigQueryTable::ReadQuery() const {
  return source_.ReadQuery(reference());
}

TableWriteRestriction BigQueryTable::WriteRestriction() const {
  return source_.WriteRestriction(reference());
}

}  // namespace bigquery_emulator_duckdb
