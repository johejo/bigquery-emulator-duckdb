#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/catalog_wrapper.h"
#include "googlesql/public/simple_catalog.h"
#include "src/field_schema.h"
#include "src/table_metadata.h"
#include "src/type_mapping.h"

namespace googlesql {
class LanguageOptions;
class Type;
class TypeFactory;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

// The language settings shared by the catalog's built-in functions and the analyzer, so that a
// statement is not rejected for a feature one of them was not told about.
const googlesql::LanguageOptions& GoogleSqlLanguageOptions();

// The schema and metadata of a table as tables.get reports them.
struct TableDescription {
  std::vector<FieldSchema> schema;
  TableMetadata metadata;
};

// Resolves a BigQuery table reference to its schema. The emulator implements it on top of the
// metadata it keeps in DuckDB.
class TableSource {
 public:
  virtual ~TableSource() = default;

  // Returns the columns of `project`.`dataset`.`table`, or nothing if there is no such table.
  virtual std::optional<std::vector<FieldSchema>> FindTable(const std::string& project,
                                                            const std::string& dataset,
                                                            const std::string& table) = 0;

  // The schema and metadata of the table `project`.`dataset`.`table`, or nothing if there is no
  // such table or it is a view. For CREATE TABLE LIKE; none by default.
  virtual std::optional<TableDescription> DescribeTable(const std::string& /*project*/,
                                                        const std::string& /*dataset*/,
                                                        const std::string& /*table*/) {
    return std::nullopt;
  }

  // The datasets of `project`, sorted by name, for INFORMATION_SCHEMA. None by default.
  virtual std::vector<std::string> ListDatasets(const std::string& /*project*/) { return {}; }

  // The tables of `project`.`dataset`, sorted by name, for INFORMATION_SCHEMA. None by default.
  virtual std::vector<std::string> ListTables(const std::string& /*project*/,
                                              const std::string& /*dataset*/) {
    return {};
  }

  // The GoogleSQL query of `project`.`dataset`.`table` if it is a view, empty for a view whose
  // query is unknown, or nothing for a table. For INFORMATION_SCHEMA; no views by default.
  virtual std::optional<std::string> FindViewQuery(const std::string& /*project*/,
                                                   const std::string& /*dataset*/,
                                                   const std::string& /*table*/) {
    return std::nullopt;
  }
};

// A table whose rows are not stored but computed by a DuckDB query, such as an
// INFORMATION_SCHEMA view. The query's columns have the table's column names.
class SqlTable : public googlesql::SimpleTable {
 public:
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a table is named before its query.
  SqlTable(const std::string& name, std::string sql)
      : googlesql::SimpleTable(name), sql_(std::move(sql)) {}

  const std::string& sql() const { return sql_; }

 private:
  std::string sql_;
};

// A table the emulator keeps, which describes itself on demand: its columns are what queries
// see, while CREATE TABLE LIKE copies the schema and metadata tables.get reports.
class BigQueryTable : public googlesql::SimpleTable {
 public:
  // `source` must outlive the table.
  BigQueryTable(TableSource& source, std::vector<std::string> path)
      : googlesql::SimpleTable(path.back()), source_(source), path_(std::move(path)) {}

  std::optional<TableDescription> Describe() const {
    return source_.DescribeTable(path_[0], path_[1], path_[2]);
  }

 private:
  TableSource& source_;
  std::vector<std::string> path_;
};

// Splits a table path into its dot-separated parts. An element with dots in it, which is how
// `project.dataset.table` in backquotes arrives, is split too; dots before the colon in a
// domain-scoped project ID are kept in the project.
std::vector<std::string> SplitTablePath(absl::Span<const std::string> path);

// Normalizes a table path to {project, dataset, table}, splitting it with SplitTablePath first.
// Missing parts come from the defaults. Returns an empty vector for a path it cannot interpret.
std::vector<std::string> NormalizeTablePath(absl::Span<const std::string> path,
                                            const std::string& default_project,
                                            const std::string& default_dataset);

// The dataset qualifier that names a temporary table explicitly, as `_SESSION.name`.
inline constexpr char kSessionDataset[] = "_SESSION";

// The temporary tables of a multi-statement query, which live in the DuckDB catalog `project`
// and its schema `dataset`: those named `names` when a statement is analyzed and translated.
struct TemporaryTables {
  std::string project;
  std::string dataset;
  std::set<std::string> names = {};
};

// The name of the temporary table that CREATE TEMP TABLE `path` creates, which is either `name`
// or `_SESSION.name`, or nothing for any other path.
std::optional<std::string> TemporaryTableName(absl::Span<const std::string> path);

// Normalizes a table path as NormalizeTablePath does, in a multi-statement query whose temporary
// tables are `temporary` when it is not null: `_SESSION.name` names a temporary table, and so
// does an unqualified name when a temporary table has it.
std::vector<std::string> ResolveTablePath(absl::Span<const std::string> path,
                                          const std::string& default_project,
                                          const std::string& default_dataset,
                                          const TemporaryTables* temporary);

// A catalog whose tables come from a TableSource and whose functions and types are the
// GoogleSQL built-ins. Tables are looked up lazily, when the analyzer first refers to them,
// and are kept for the lifetime of the catalog, so use one catalog per statement: a table
// changed by a later DDL statement would otherwise still be served with its old schema.
class BigQueryCatalog : public googlesql::CatalogWrapper {
 public:
  // `source`, `type_factory` and `temporary`, the temporary tables of the multi-statement query
  // that the catalog analyzes a statement of, if any, must outlive the catalog.
  BigQueryCatalog(TableSource& source, googlesql::TypeFactory* type_factory,
                  std::string default_project, std::string default_dataset,
                  const TemporaryTables* temporary = nullptr);
  ~BigQueryCatalog() override;

  std::string FullName() const override { return "bigquery"; }

  absl::Status FindTable(const absl::Span<const std::string>& path, const googlesql::Table** table,
                         const FindOptions& options) override;

 private:
  TableSource& source_;
  googlesql::TypeFactory* type_factory_;
  std::string default_project_;
  std::string default_dataset_;
  const TemporaryTables* temporary_;
  // Keyed by the normalized path. The analyzer holds on to the Table pointers it is handed,
  // so they must stay valid for as long as the catalog does.
  std::map<std::vector<std::string>, std::unique_ptr<googlesql::SimpleTable>> tables_;
};

}  // namespace bigquery_emulator_duckdb
