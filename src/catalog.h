#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/catalog_wrapper.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/type.h"
#include "src/field_schema.h"

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
};

// Normalizes a table path to {project, dataset, table}. An element with dots in it, which is how
// `project.dataset.table` in backquotes arrives, is split first; a missing project and dataset
// are then taken from the defaults. Returns an empty vector for a path it cannot interpret.
std::vector<std::string> NormalizeTablePath(absl::Span<const std::string> path,
                                            const std::string& default_project,
                                            const std::string& default_dataset);

// Maps a BigQuery TableFieldSchema to the GoogleSQL type of the column; the GoogleSQL
// counterpart of the emulator's DuckDB type mapping.
absl::StatusOr<const googlesql::Type*> GoogleSqlType(const FieldSchema& field,
                                                     googlesql::TypeFactory* type_factory);

// A catalog whose tables come from a TableSource and whose functions and types are the
// GoogleSQL built-ins. Tables are looked up lazily, when the analyzer first refers to them,
// and are kept for the lifetime of the catalog, so use one catalog per statement: a table
// changed by a later DDL statement would otherwise still be served with its old schema.
class BigQueryCatalog : public googlesql::CatalogWrapper {
 public:
  // `source` and `type_factory` must outlive the catalog.
  BigQueryCatalog(TableSource& source, googlesql::TypeFactory* type_factory,
                  std::string default_project, std::string default_dataset);

  std::string FullName() const override { return "bigquery"; }

  absl::Status FindTable(const absl::Span<const std::string>& path, const googlesql::Table** table,
                         const FindOptions& options = FindOptions()) override;

 private:
  TableSource& source_;
  googlesql::TypeFactory* type_factory_;
  std::string default_project_;
  std::string default_dataset_;
  // Keyed by the normalized path. The analyzer holds on to the Table pointers it is handed,
  // so they must stay valid for as long as the catalog does.
  std::map<std::vector<std::string>, std::unique_ptr<googlesql::SimpleTable>> tables_;
};

}  // namespace bigquery_emulator_duckdb
