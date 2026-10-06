#include "src/catalog.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "googlesql/public/builtin_function_options.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/function.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/type.h"
#include "src/field_schema.h"
#include "src/information_schema.h"

namespace bigquery_emulator_duckdb {
namespace {

// Built once per process: registering every built-in function is expensive, and SimpleCatalog
// lookups are thread-safe.
googlesql::SimpleCatalog* BuiltinCatalog() {
  static googlesql::SimpleCatalog* const kCatalog = [] {
    auto* catalog = new googlesql::SimpleCatalog("builtin", new googlesql::TypeFactory());
    const absl::Status status = catalog->AddBuiltinFunctionsAndTypes(
        googlesql::BuiltinFunctionOptions(GoogleSqlLanguageOptions()));
    if (!status.ok()) {
      throw std::runtime_error(status.ToString());
    }
    // BigQuery functions that GoogleSQL does not ship, declared with the signatures the
    // translator supports.
    catalog->AddOwnedFunction(new googlesql::Function(
        "contains_substr", "bigquery", googlesql::Function::SCALAR,
        {googlesql::FunctionSignature(
            googlesql::FunctionArgumentType(googlesql::types::BoolType()),
            {googlesql::FunctionArgumentType(googlesql::types::StringType()),
             googlesql::FunctionArgumentType(googlesql::types::StringType())},
            /*context_id=*/static_cast<int64_t>(0))}));
    for (const char* name : {"max_by", "min_by"}) {
      catalog->AddOwnedFunction(new googlesql::Function(
          name, "bigquery", googlesql::Function::AGGREGATE,
          {googlesql::FunctionSignature(
              googlesql::FunctionArgumentType(googlesql::ARG_KIND_EXPR_ANY_1),
              {googlesql::FunctionArgumentType(googlesql::ARG_KIND_EXPR_ANY_1),
               googlesql::FunctionArgumentType(googlesql::ARG_KIND_EXPR_ANY_2)},
              /*context_id=*/static_cast<int64_t>(0))},
          googlesql::FunctionOptions(googlesql::FunctionOptions::ORDER_OPTIONAL,
                                     /*window_framing_support_in=*/true)));
    }
    return catalog;
  }();
  return kCatalog;
}

}  // namespace

const googlesql::LanguageOptions& GoogleSqlLanguageOptions() {
  static const googlesql::LanguageOptions* const kLanguageOptions = [] {
    auto* options = new googlesql::LanguageOptions();
    // Some syntax BigQuery accepts is gated behind a language feature that the default options
    // leave off, QUALIFY among it, so every released feature is turned on. Accepting a little
    // more than BigQuery does is the lesser problem for an emulator: a query the parser rejects
    // cannot run at all.
    options->EnableMaximumLanguageFeatures();
    // Some of those features add types and precision that BigQuery lacks rather than syntax, so
    // they are turned off again: BigQuery has no MAP, UUID or FLOAT32, and its timestamps hold
    // microseconds, not nanoseconds or picoseconds.
    for (const googlesql::LanguageFeature feature :
         {googlesql::FEATURE_MAP_TYPE, googlesql::FEATURE_UUID_TYPE,
          googlesql::FEATURE_TIMESTAMP_NANOS, googlesql::FEATURE_TIMESTAMP_PICOS}) {
      options->DisableLanguageFeature(feature);
    }
    options->EnableLanguageFeature(googlesql::FEATURE_DISABLE_FLOAT32);
    // BigQuery has EDIT_DISTANCE over BYTES and CREATE TABLE CLONE, which GoogleSQL still marks
    // in development.
    options->EnableLanguageFeature(googlesql::FEATURE_ENABLE_EDIT_DISTANCE_BYTES);
    options->EnableLanguageFeature(googlesql::FEATURE_CREATE_TABLE_CLONE);
    // BigQuery is the external product: INT64 and FLOAT64 rather than the internal type set.
    options->set_product_mode(googlesql::PRODUCT_EXTERNAL);
    // The analyzer accepts only queries by default, but the emulator also runs DDL and DML.
    options->SetSupportsAllStatementKinds();
    return options;
  }();
  return *kLanguageOptions;
}

std::vector<std::string> SplitTablePath(absl::Span<const std::string> path) {
  std::vector<std::string> parts;
  for (const std::string& element : path) {
    for (const absl::string_view part : absl::StrSplit(element, '.')) {
      // Each element expands into several owned strings, rather than a direct copy.
      // cppcheck-suppress useStlAlgorithm
      parts.emplace_back(part);
    }
  }
  // Domain-scoped project IDs contain dots before the colon. Those dots belong to the
  // project, even when GoogleSQL passes the entire backtick-quoted path as one element.
  const auto project_part = std::ranges::find_if(
      parts, [](const std::string& part) { return part.find(':') != std::string::npos; });
  if (project_part != parts.end() && project_part != parts.begin()) {
    const auto project_end = std::next(project_part);
    const std::string project = absl::StrJoin(parts.begin(), project_end, ".");
    parts.erase(parts.begin(), project_end);
    parts.insert(parts.begin(), project);
  }
  return parts;
}

std::vector<std::string> NormalizeTablePath(absl::Span<const std::string> path,
                                            const std::string& default_project,
                                            const std::string& default_dataset) {
  std::vector<std::string> parts = SplitTablePath(path);
  if (parts.size() == 1) {
    parts.insert(parts.begin(), {default_project, default_dataset});
  } else if (parts.size() == 2) {
    parts.insert(parts.begin(), default_project);
  }
  if (parts.size() != 3) {
    return {};
  }
  if (std::ranges::any_of(parts, [](const std::string& part) { return part.empty(); })) {
    return {};
  }
  return parts;
}

std::optional<std::string> TemporaryTableName(absl::Span<const std::string> path) {
  const std::vector<std::string> parts = SplitTablePath(path);
  if (parts.size() == 1 && !parts[0].empty()) {
    return parts[0];
  }
  if (parts.size() == 2 && parts[0] == kSessionDataset && !parts[1].empty()) {
    return parts[1];
  }
  return std::nullopt;
}

std::vector<std::string> ResolveTablePath(absl::Span<const std::string> path,
                                          const std::string& default_project,
                                          const std::string& default_dataset,
                                          const TemporaryTables* temporary) {
  if (temporary != nullptr) {
    const std::vector<std::string> parts = SplitTablePath(path);
    const bool session = parts.size() == 2 && parts[0] == kSessionDataset;
    if (session || (parts.size() == 1 && temporary->names.contains(parts[0]))) {
      return {temporary->project, temporary->dataset, parts.back()};
    }
  }
  return NormalizeTablePath(path, default_project, default_dataset);
}

BigQueryCatalog::BigQueryCatalog(TableSource& source, googlesql::TypeFactory* type_factory,
                                 std::string default_project, std::string default_dataset,
                                 const TemporaryTables* temporary)
    : googlesql::CatalogWrapper(BuiltinCatalog()),
      source_(source),
      type_factory_(type_factory),
      default_project_(std::move(default_project)),
      default_dataset_(std::move(default_dataset)),
      temporary_(temporary) {}

BigQueryCatalog::~BigQueryCatalog() = default;

absl::Status BigQueryCatalog::FindTable(const absl::Span<const std::string>& path,
                                        const googlesql::Table** table,
                                        const FindOptions& /*options*/) {
  *table = nullptr;
  if (const std::vector<std::string> parts = SplitTablePath(path); IsInformationSchemaPath(parts)) {
    // Keyed with an empty first part, which no normalized table path has.
    std::vector<std::string> key = {""};
    key.insert(key.end(), parts.begin(), parts.end());
    if (auto it = tables_.find(key); it != tables_.end()) {
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
      ResolveTablePath(path, default_project_, default_dataset_, temporary_);
  if (normalized.empty()) {
    return absl::NotFoundError("Table not found: " + absl::StrJoin(path, "."));
  }
  if (auto it = tables_.find(normalized); it != tables_.end()) {
    *table = it->second.get();
    return absl::OkStatus();
  }

  const std::optional<std::vector<FieldSchema>> schema =
      source_.FindTable(normalized[0], normalized[1], normalized[2]);
  if (!schema.has_value()) {
    return absl::NotFoundError("Table not found: " + absl::StrJoin(normalized, "."));
  }
  auto simple_table = std::make_unique<BigQueryTable>(source_, normalized);
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
            std::make_unique<googlesql::SimpleColumn>(normalized[2], field.name, *type));
        !status.ok()) {
      return status;
    }
  }
  *table = simple_table.get();
  tables_.emplace(normalized, std::move(simple_table));
  return absl::OkStatus();
}

}  // namespace bigquery_emulator_duckdb
