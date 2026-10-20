#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "src/field_schema.h"
#include "src/references.h"
#include "src/routine.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

// Resolves a BigQuery table reference to its schema. The emulator implements it on top of the
// metadata it keeps in DuckDB.
class TableSource {
 public:
  virtual ~TableSource() = default;

  // Returns the columns of `project`.`dataset`.`table`, or nothing if there is no such table.
  virtual std::optional<std::vector<FieldSchema>> FindTable(const std::string& project,
                                                            const std::string& dataset,
                                                            const std::string& table) = 0;

  // A query that converts a table's external numeric storage to query values, or nullopt for
  // the emulator's native storage. A table that needs this conversion is read-only.
  virtual std::optional<std::string> NumericReadQuery(const TableReference& /*table*/) {
    return std::nullopt;
  }

  // The schema and metadata of the table `project`.`dataset`.`table`, or nothing if there is no
  // such table or it is a view. For CREATE TABLE LIKE, COPY and CLONE and INFORMATION_SCHEMA;
  // none by default.
  virtual std::optional<TableDescription> DescribeTable(const std::string& /*project*/,
                                                        const std::string& /*dataset*/,
                                                        const std::string& /*table*/) {
    return std::nullopt;
  }

  // The metadata of the dataset `project`.`dataset`, or nothing if there is no such dataset. For
  // the default rounding mode that CREATE TABLE AS SELECT rounds with and INFORMATION_SCHEMA;
  // none by default.
  virtual std::optional<DatasetMetadata> DescribeDataset(const std::string& /*project*/,
                                                         const std::string& /*dataset*/) {
    return std::nullopt;
  }

  // The datasets of `project`, sorted by name, for INFORMATION_SCHEMA. None by default.
  virtual std::vector<std::string> ListDatasets(const std::string& /*project*/) { return {}; }

  // Whether the emulator holds the project `project`, for INFORMATION_SCHEMA. Every project by
  // default.
  virtual bool HasProject(const std::string& /*project*/) { return true; }

  // The ID of the project that `project` names, by its ID or by its number. `project` itself by
  // default, and for a project the emulator does not hold.
  [[nodiscard]] virtual std::string ProjectId(const std::string& project) const { return project; }

  // The location of the dataset `project`.`dataset`, or nothing if there is no such dataset, for
  // INFORMATION_SCHEMA. Unlike the lookups above, it does not count as the job using the dataset.
  // Nothing by default.
  virtual std::optional<std::string> DatasetLocation(const std::string& /*project*/,
                                                     const std::string& /*dataset*/) {
    return std::nullopt;
  }

  // Records that the job uses `name`, such as an INFORMATION_SCHEMA view of a region, which is in
  // `location`, as JobLocation::Reference does. Does nothing by default.
  virtual void ReferenceLocation(const std::string& /*name*/, const std::string& /*location*/) {}

  // The tables of `project`.`dataset`, sorted by name, for INFORMATION_SCHEMA. None by default.
  virtual std::vector<std::string> ListTables(const std::string& /*project*/,
                                              const std::string& /*dataset*/) {
    return {};
  }

  // The number of rows of `table` for each value of `key`, a DuckDB expression of type VARCHAR
  // over its columns, for INFORMATION_SCHEMA.PARTITIONS; nothing if there is no such table. Groups
  // without rows are left out. Nothing by default.
  virtual std::optional<std::vector<std::pair<std::optional<std::string>, int64_t>>> CountRows(
      const TableReference& /*table*/, const std::string& /*key*/) {
    return std::nullopt;
  }

  // The view `project`.`dataset`.`table`, or nothing for a table. For INFORMATION_SCHEMA; no
  // views by default.
  virtual std::optional<ViewDescription> FindView(const std::string& /*project*/,
                                                  const std::string& /*dataset*/,
                                                  const std::string& /*table*/) {
    return std::nullopt;
  }

  // The persistent routines of a dataset, sorted by name. None by default.
  virtual std::vector<Routine> ListRoutines(const DatasetReference& /*dataset*/) { return {}; }

  // The persistent UDF `routine`, or nothing if there is no such routine. None by default.
  virtual std::optional<Routine> FindRoutine(const RoutineReference& /*routine*/) {
    return std::nullopt;
  }

  // The columns that EXTERNAL_QUERY returns for `query` on the connection `key`, as
  // ConnectionKey gives it, with the given JSON options, once its DuckDB
  // table macro ExternalDatabaseName(key) is registered. An error status for a connection that does
  // not exist or a query that EXTERNAL_QUERY rejects. No connections by default.
  virtual absl::StatusOr<std::vector<FieldSchema>> DescribeExternalQuery(
      const std::string& key, const std::string& /*query*/, const std::string& /*options*/) {
    return absl::NotFoundError("Not found: Connection " + key);
  }
};

}  // namespace bigquery_emulator_duckdb
