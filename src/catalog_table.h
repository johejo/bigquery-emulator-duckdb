#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/simple_catalog.h"
#include "src/field_schema.h"
#include "src/references.h"
#include "src/statement_resources.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

class TableSource;

// A table whose rows are not stored but computed by a DuckDB query, such as an
// INFORMATION_SCHEMA view. The query's columns have the table's column names.
class SqlTable : public googlesql::SimpleTable {
 public:
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a table is named before its query.
  SqlTable(const std::string& name, std::string sql)
      : googlesql::SimpleTable(name), sql_(std::move(sql)) {}

  [[nodiscard]] const std::string& sql() const { return sql_; }

 private:
  std::string sql_;
};

// A table the emulator keeps, which describes itself on demand: its columns are what queries
// see, while CREATE TABLE LIKE, COPY and CLONE copy the schema and metadata tables.get reports.
class BigQueryTable : public googlesql::SimpleTable {
 public:
  // `source` must outlive the table, whose columns are those of `schema`, in order.
  BigQueryTable(TableSource& source, std::vector<std::string> path, std::vector<FieldSchema> schema)
      : googlesql::SimpleTable(path.back()),
        source_(source),
        path_(std::move(path)),
        schema_(std::move(schema)) {}

  [[nodiscard]] std::optional<TableDescription> Describe() const;
  // The columns of the table's primary key, which graph element tables without a KEY clause take.
  [[nodiscard]] std::optional<std::vector<int>> PrimaryKey() const override;
  [[nodiscard]] std::optional<TableRead> ReadQuery() const;
  [[nodiscard]] TableWriteRestriction WriteRestriction() const;
  [[nodiscard]] TableReference reference() const {
    return {.project_id = path_.at(0), .dataset_id = path_.at(1), .table_id = path_.at(2)};
  }
  // The field of column `index`, such as for its rounding mode.
  [[nodiscard]] const FieldSchema& field(int index) const {
    return schema_.at(static_cast<size_t>(index));
  }

 private:
  TableSource& source_;
  std::vector<std::string> path_;
  std::vector<FieldSchema> schema_;
};

}  // namespace bigquery_emulator_duckdb
