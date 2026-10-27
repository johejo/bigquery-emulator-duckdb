#include "src/catalog.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "googlesql/public/analyzer_output.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/type.h"
#include "src/analyzer_functions/functions.h"
#include "src/builtin_catalog.h"
#include "src/builtin_function_options.h"
#include "src/catalog_table.h"
#include "src/field_schema.h"
#include "src/information_schema.h"
#include "src/table_path.h"
#include "src/table_source.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {

BigQueryCatalog::BigQueryCatalog(TableSource& source, googlesql::TypeFactory* type_factory,
                                 std::string default_project, std::string default_dataset,
                                 const TemporaryTables* temporary,
                                 const ExcludedFunctionIds& excluded_function_ids,
                                 FunctionDeclarations declarations)
    : googlesql::CatalogWrapper(BuiltinCatalog(excluded_function_ids, declarations)),
      excluded_function_ids_(excluded_function_ids),
      declarations_(declarations),
      source_(source),
      type_factory_(type_factory),
      default_project_(std::move(default_project)),
      default_dataset_(std::move(default_dataset)),
      temporary_(temporary) {}

BigQueryCatalog::~BigQueryCatalog() = default;

absl::Status BigQueryCatalog::FindTableValuedFunction(
    const absl::Span<const std::string>& path, const googlesql::TableValuedFunction** function,
    const FindOptions& options) {
  if (declarations_ != FunctionDeclarations::kBigQuery || path.size() != 1 ||
      !absl::EqualsIgnoreCase(path.front(), "external_query")) {
    return googlesql::CatalogWrapper::FindTableValuedFunction(path, function, options);
  }
  if (external_query_ == nullptr) {
    external_query_ =
        analyzer_functions::MakeExternalQueryFunction(source_, default_project_, type_factory_);
  }
  *function = external_query_.get();
  return absl::OkStatus();
}

absl::Status BigQueryCatalog::FindTable(const absl::Span<const std::string>& path,
                                        const googlesql::Table** table,
                                        const FindOptions& /*options*/) {
  *table = nullptr;
  if (const std::vector<std::string> parts = SplitTablePath(path); IsInformationSchemaPath(parts)) {
    // Keyed with an empty first part, which no normalized table path has.
    std::vector<std::string> key = {""};
    key.insert(key.end(), parts.begin(), parts.end());
    if (auto const it = tables_.find(key); it != tables_.end()) {
      *table = it->second.get();
      return absl::OkStatus();
    }
    absl::StatusOr<std::unique_ptr<SqlTable>> view =
        InformationSchemaView(parts, source_, type_factory_, default_project_, default_dataset_);
    if (!view.ok()) {
      return view.status();
    }
    *table = view->get();
    tables_.emplace(std::move(key), *std::move(view));
    return absl::OkStatus();
  }
  const std::vector<std::string> normalized =
      ResolveTablePath(path, default_project_, default_dataset_, temporary_, &source_);
  if (normalized.empty()) {
    return absl::NotFoundError("Table not found: " + absl::StrJoin(path, "."));
  }
  if (auto const it = tables_.find(normalized); it != tables_.end()) {
    *table = it->second.get();
    return absl::OkStatus();
  }

  const std::optional<std::vector<FieldSchema>> schema =
      source_.FindTable(normalized.at(0), normalized.at(1), normalized.at(2));
  if (!schema.has_value()) {
    return absl::NotFoundError("Table not found: " + absl::StrJoin(normalized, "."));
  }
  auto simple_table = std::make_unique<BigQueryTable>(source_, normalized, *schema);
  if (absl::Status status = simple_table->set_full_name(absl::StrJoin(normalized, "."));
      !status.ok()) {
    return status;
  }
  for (const FieldSchema& field : *schema) {
    absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(field, type_factory_);
    if (!type.ok()) {
      return type.status();
    }
    if (absl::Status status = simple_table->AddColumn(
            std::make_unique<googlesql::SimpleColumn>(normalized.at(2), field.name, *type));
        !status.ok()) {
      return status;
    }
  }
  *table = simple_table.get();
  tables_.emplace(normalized, std::move(simple_table));
  return absl::OkStatus();
}

absl::Status BigQueryCatalog::FindTableWithPathPrefix(absl::Span<const std::string> path,
                                                      const FindOptions& options,
                                                      int* num_names_consumed,
                                                      const googlesql::Table** table) {
  absl::Status status =
      googlesql::Catalog::FindTableWithPathPrefix(path, options, num_names_consumed, table);
  // GoogleSQL also looks up prefixes of qualified names. Report a missing dataset only for
  // an unqualified original name, after those lookups have failed.
  if (absl::IsNotFound(status) && default_dataset_.empty() && SplitTablePath(path).size() == 1) {
    return absl::InvalidArgumentError("Table \"" + path.front() +
                                      "\" must be qualified with a dataset (e.g. dataset.table).");
  }
  return status;
}

}  // namespace bigquery_emulator_duckdb
