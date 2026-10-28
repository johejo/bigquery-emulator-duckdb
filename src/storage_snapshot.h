#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "src/field_schema.h"
#include "src/statement_resources.h"

struct duckdb_result;

namespace bigquery_emulator_duckdb {

class Backend;

// The rows of a Storage Read session, kept as the DuckDB vectors of a query result. Each read
// encodes its rows: columns whose DuckDB Arrow layout is the one Storage Read gives are exported
// through the Arrow C Data interface, and other columns are appended from their typed vectors.
// Both Arrow and Avro encode the resulting arrays.
class StorageSnapshot {
 public:
  // Fetches every row of `result`, whose columns are the fields of `schema`. The session keeps
  // the connection that Arrow export options name alive.
  StorageSnapshot(duckdb_result& result, std::vector<FieldSchema> schema,
                  std::shared_ptr<Backend> session,
                  std::shared_ptr<const StatementResources> resources = {});
  ~StorageSnapshot();
  StorageSnapshot(const StorageSnapshot&) = delete;
  StorageSnapshot& operator=(const StorageSnapshot&) = delete;

  [[nodiscard]] const std::vector<FieldSchema>& schema() const;
  [[nodiscard]] size_t row_count() const;
  // Rows [offset, offset + count) as one encapsulated Arrow IPC record batch message, or as Avro
  // binary rows. count must fit in a DuckDB vector.
  [[nodiscard]] std::string ReadRows(size_t offset, size_t count, bool arrow) const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bigquery_emulator_duckdb
