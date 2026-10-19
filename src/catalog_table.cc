#include "src/catalog_table.h"

#include <optional>

#include "src/table_metadata.h"
#include "src/table_source.h"

namespace bigquery_emulator_duckdb {

std::optional<TableDescription> BigQueryTable::Describe() const {
  return source_.DescribeTable(path_.at(0), path_.at(1), path_.at(2));
}

}  // namespace bigquery_emulator_duckdb
