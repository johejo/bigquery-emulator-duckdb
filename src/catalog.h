#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/types/span.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/catalog_wrapper.h"
#include "googlesql/public/simple_catalog.h"
#include "src/builtin_function_options.h"
#include "src/timestamp_precision.h"

namespace googlesql {
class TableValuedFunction;
class AnalyzerOutput;
class TypeFactory;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

class TableSource;
struct TemporaryTables;

// A catalog whose tables come from a TableSource and whose functions and types are the
// GoogleSQL built-ins plus BigQuery-specific declarations. Tables are looked up lazily,
// when the analyzer first refers to them, and kept for its lifetime. Use one per statement: a table
// changed by a later DDL statement would otherwise still be served with its old schema.
class BigQueryCatalog : public googlesql::CatalogWrapper {
 public:
  // `source`, `type_factory` and `temporary`, the temporary tables of the multi-statement query
  // that the catalog analyzes a statement of, if any, must outlive the catalog. The catalog
  // analyzes the views, routines and property graphs it resolves with `timestamp_precision`.
  BigQueryCatalog(TableSource& source, googlesql::TypeFactory* type_factory,
                  std::string default_project, std::string default_dataset,
                  TimestampPrecision timestamp_precision, const TemporaryTables* temporary,
                  const ExcludedFunctionIds& excluded_function_ids,
                  FunctionDeclarations declarations);
  ~BigQueryCatalog() override;

  [[nodiscard]] std::string FullName() const override { return "bigquery"; }

  [[nodiscard]] TimestampPrecision timestamp_precision() const { return timestamp_precision_; }

  absl::Status FindTable(const absl::Span<const std::string>& path, const googlesql::Table** table,
                         const FindOptions& options) override;
  absl::Status FindTableWithPathPrefix(absl::Span<const std::string> path,
                                       const FindOptions& options, int* num_names_consumed,
                                       const googlesql::Table** table) override;
  // EXTERNAL_QUERY, whose columns depend on the database it queries, alongside the built-ins.
  using googlesql::Catalog::FindTableValuedFunction;
  absl::Status FindTableValuedFunction(const absl::Span<const std::string>& path,
                                       const googlesql::TableValuedFunction** function,
                                       const FindOptions& options) override;

  absl::Status FindPropertyGraph(absl::Span<const std::string> path,
                                 const googlesql::PropertyGraph*& graph,
                                 const FindOptions& options) override;

 protected:
  [[nodiscard]] TableSource& source() const { return source_; }
  [[nodiscard]] googlesql::TypeFactory* type_factory() const { return type_factory_; }
  [[nodiscard]] const std::string& default_project() const { return default_project_; }

  [[nodiscard]] const ExcludedFunctionIds& excluded_function_ids() const {
    return excluded_function_ids_;
  }

  [[nodiscard]] FunctionDeclarations function_declarations() const { return declarations_; }

 private:
  ExcludedFunctionIds excluded_function_ids_;
  FunctionDeclarations declarations_;
  TableSource& source_;
  googlesql::TypeFactory* type_factory_;
  std::string default_project_;
  std::string default_dataset_;
  TimestampPrecision timestamp_precision_;
  const TemporaryTables* temporary_;
  // Keyed by the normalized path. The analyzer holds on to the Table pointers it is handed,
  // so they must stay valid for as long as the catalog does.
  std::map<std::vector<std::string>, std::unique_ptr<googlesql::SimpleTable>> tables_;
  std::unique_ptr<googlesql::TableValuedFunction> external_query_;
  // Destroy graphs before their analyzer outputs and the tables they borrow.
  std::vector<std::unique_ptr<const googlesql::AnalyzerOutput>> graph_artifacts_;
  std::map<std::vector<std::string>, std::unique_ptr<googlesql::SimpleCatalog>> graphs_;
};

}  // namespace bigquery_emulator_duckdb
