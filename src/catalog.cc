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
#include "absl/strings/match.h"
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
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "src/external_query.h"
#include "src/field_schema.h"
#include "src/information_schema.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {
namespace {

// The group of the functions the catalog declares because GoogleSQL does not ship them, which
// BigQuery names in errors such as "No matching signature for function BIGQUERYGOOGLESQL:...".
constexpr char kBigQueryGroup[] = "BigQueryGoogleSQL";

// BigQuery's RANGE_SESSIONIZE is not in GoogleSQL's built-in catalog. Its output
// retains the input schema and appends a RANGE of the selected column's type.
absl::StatusOr<std::shared_ptr<googlesql::TVFSignature>> SessionizeSchema(
    googlesql::Catalog* /*catalog*/, googlesql::TypeFactory* /*type_factory*/,
    const googlesql::FunctionSignature& /*signature*/,
    const std::vector<googlesql::TVFInputArgumentType>& arguments,
    const googlesql::AnalyzerOptions& /*options*/) {
  const auto& input = arguments.at(0).relation();
  if (input.is_value_table()) {
    return absl::InvalidArgumentError("The emulator does not support RANGE_SESSIONIZE value table");
  }
  std::vector<googlesql::Value> literals;
  for (size_t i = 1; i < arguments.size(); ++i) {
    const auto scalar = arguments.at(i).GetScalarArgType();
    if (!scalar.ok() || scalar->literal_value() == nullptr || scalar->literal_value()->is_null()) {
      return absl::InvalidArgumentError(
          "The emulator does not support RANGE_SESSIONIZE non-literal or NULL argument");
    }
    literals.push_back(*scalar->literal_value());
  }
  const auto find = [&](const std::string& name) -> const googlesql::TVFRelation::Column* {
    const googlesql::TVFRelation::Column* found = nullptr;
    for (const auto& column : input.columns()) {
      if (absl::EqualsIgnoreCase(column.name, name)) {
        if (found != nullptr) {
          return nullptr;
        }
        found = &column;
      }
    }
    return found;
  };
  const auto* range = find(literals.at(0).string_value());
  if (range == nullptr || !range->type->IsRange()) {
    return absl::InvalidArgumentError("RANGE_SESSIONIZE requires an unambiguous RANGE column");
  }
  const auto partition_names = literals.at(1).elements_view();
  for (const auto& name : *partition_names) {
    const auto* column = name.is_null() ? nullptr : find(name.string_value());
    if (column == nullptr) {
      return absl::InvalidArgumentError(
          "RANGE_SESSIONIZE requires unambiguous partitioning columns");
    }
    if (!column->type->SupportsGrouping(GoogleSqlLanguageOptions())) {
      return absl::InvalidArgumentError(
          "The emulator does not support RANGE_SESSIONIZE partitioning column type");
    }
  }
  if (literals.size() == 3 && literals.at(2).EnumDisplayName() != "MEETS" &&
      literals.at(2).EnumDisplayName() != "OVERLAPS") {
    return absl::InvalidArgumentError("RANGE_SESSIONIZE option must be MEETS or OVERLAPS");
  }
  auto columns = input.columns();
  bool replaced = false;
  for (auto& column : columns) {
    if (absl::EqualsIgnoreCase(column.name, "session_range")) {
      column = googlesql::TVFRelation::Column("session_range", range->type);
      replaced = true;
    }
  }
  if (!replaced) {
    columns.emplace_back("session_range", range->type);
  }
  return googlesql::TVFSignature::Create(arguments, googlesql::TVFRelation(std::move(columns)));
}

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
    catalog->AddOwnedTableValuedFunction(std::make_unique<googlesql::TableValuedFunction>(
        std::vector<std::string>{"range_sessionize"}, kBigQueryGroup,
        std::vector<googlesql::FunctionSignature>{
            googlesql::FunctionSignature(
                googlesql::ARG_KIND_RELATION,
                {
                    googlesql::FunctionArgumentType::AnyRelation(),
                    googlesql::FunctionArgumentType(googlesql::types::StringType()),
                    googlesql::FunctionArgumentType(googlesql::types::StringArrayType()),
                    googlesql::FunctionArgumentType(googlesql::types::RangeSessionizeModeEnumType(),
                                                    googlesql::FunctionArgumentType::OPTIONAL),
                },
                /*context_id=*/int64_t{0}),
        },
        googlesql::TableValuedFunctionOptions().set_compute_result_type_callback(
            SessionizeSchema)));
    // BigQuery functions that GoogleSQL does not ship, declared with the signatures the
    // translator supports.
    catalog->AddOwnedFunction(new googlesql::Function(
        "contains_substr", kBigQueryGroup, googlesql::Function::SCALAR,
        {
            googlesql::FunctionSignature(
                googlesql::FunctionArgumentType(googlesql::types::BoolType()),
                {
                    googlesql::FunctionArgumentType(googlesql::types::StringType()),
                    googlesql::FunctionArgumentType(googlesql::types::StringType()),
                },
                /*context_id=*/static_cast<int64_t>(0)),
        }));
    // TEXT_ANALYZE's analyzer and analyzer_options are named only. Their defaults keep an omitted
    // one apart from an explicit NULL, which makes the result NULL; options of {} are no options.
    googlesql::FunctionArgumentTypeOptions analyzer(googlesql::FunctionArgumentType::OPTIONAL);
    analyzer.set_argument_name("analyzer", googlesql::kNamedOnly);
    analyzer.set_default(googlesql::Value::String("LOG_ANALYZER"));
    googlesql::FunctionArgumentTypeOptions analyzer_options(
        googlesql::FunctionArgumentType::OPTIONAL);
    analyzer_options.set_argument_name("analyzer_options", googlesql::kNamedOnly);
    analyzer_options.set_default(googlesql::Value::String("{}"));
    catalog->AddOwnedFunction(new googlesql::Function(
        "text_analyze", kBigQueryGroup, googlesql::Function::SCALAR,
        {
            googlesql::FunctionSignature(
                googlesql::FunctionArgumentType(googlesql::types::StringArrayType()),
                {
                    googlesql::FunctionArgumentType(googlesql::types::StringType()),
                    googlesql::FunctionArgumentType(googlesql::types::StringType(), analyzer),
                    googlesql::FunctionArgumentType(googlesql::types::StringType(),
                                                    analyzer_options),
                },
                /*context_id=*/static_cast<int64_t>(0)),
        }));
    const googlesql::Type* term = nullptr;
    if (!catalog->type_factory()
             ->MakeStructType(
                 {
                     {"term", googlesql::types::StringType()},
                     {"count", googlesql::types::Int64Type()},
                 },
                 &term)
             .ok()) {
      throw std::runtime_error("failed to make the type of BAG_OF_WORDS");
    }
    const absl::StatusOr<const googlesql::ArrayType*> terms =
        catalog->type_factory()->MakeArrayType(term);
    if (!terms.ok()) {
      throw std::runtime_error(terms.status().ToString());
    }
    catalog->AddOwnedFunction(new googlesql::Function(
        "bag_of_words", kBigQueryGroup, googlesql::Function::SCALAR,
        {
            googlesql::FunctionSignature(
                googlesql::FunctionArgumentType(*terms),
                {googlesql::FunctionArgumentType(googlesql::types::StringArrayType())},
                /*context_id=*/static_cast<int64_t>(0)),
        }));
    // TF_IDF takes only OVER (); the translator rejects PARTITION BY and SAFE as BigQuery does. The
    // analyzer checks literal limits, and the backend query parameters.
    const googlesql::Type* score = nullptr;
    if (!catalog->type_factory()
             ->MakeStructType(
                 {
                     {"term", googlesql::types::StringType()},
                     {"tf_idf", googlesql::types::DoubleType()},
                 },
                 &score)
             .ok()) {
      throw std::runtime_error("failed to make the type of TF_IDF");
    }
    const absl::StatusOr<const googlesql::ArrayType*> scores =
        catalog->type_factory()->MakeArrayType(score);
    if (!scores.ok()) {
      throw std::runtime_error(scores.status().ToString());
    }
    googlesql::FunctionArgumentTypeOptions max_distinct_tokens(
        googlesql::FunctionArgumentType::OPTIONAL);
    googlesql::FunctionArgumentTypeOptions frequency_threshold(
        googlesql::FunctionArgumentType::OPTIONAL);
    // Only the legacy constness reports "must be a literal or query parameter", as BigQuery does.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    max_distinct_tokens.set_must_be_constant();
    frequency_threshold.set_must_be_constant();
#pragma clang diagnostic pop
    max_distinct_tokens.set_must_be_non_null().set_min_value(1).set_max_value(1048576);
    frequency_threshold.set_must_be_non_null().set_min_value(0);
    catalog->AddOwnedFunction(new googlesql::Function(
        "tf_idf", kBigQueryGroup, googlesql::Function::ANALYTIC,
        {
            googlesql::FunctionSignature(
                googlesql::FunctionArgumentType(*scores),
                {
                    googlesql::FunctionArgumentType(googlesql::types::StringArrayType()),
                    googlesql::FunctionArgumentType(googlesql::types::Int64Type(),
                                                    max_distinct_tokens),
                    googlesql::FunctionArgumentType(googlesql::types::Int64Type(),
                                                    frequency_threshold),
                },
                /*context_id=*/static_cast<int64_t>(0)),
        },
        googlesql::FunctionOptions(googlesql::FunctionOptions::ORDER_UNSUPPORTED,
                                   /*window_framing_support_in=*/false)));
    for (const char* name : {"max_by", "min_by"}) {
      catalog->AddOwnedFunction(new googlesql::Function(
          name, kBigQueryGroup, googlesql::Function::AGGREGATE,
          {
              googlesql::FunctionSignature(
                  googlesql::FunctionArgumentType(googlesql::ARG_KIND_EXPR_ANY_1),
                  {
                      googlesql::FunctionArgumentType(googlesql::ARG_KIND_EXPR_ANY_1),
                      googlesql::FunctionArgumentType(googlesql::ARG_KIND_EXPR_ANY_2),
                  },
                  /*context_id=*/static_cast<int64_t>(0)),
          },
          googlesql::FunctionOptions(googlesql::FunctionOptions::ORDER_OPTIONAL,
                                     /*window_framing_support_in=*/true)));
    }
    return catalog;
  }();
  return kCatalog;
}

}  // namespace

bool IsBuiltinFunction(const googlesql::Function& function) {
  return function.IsGoogleSQLBuiltin() || function.GetGroup() == kBigQueryGroup;
}

const googlesql::LanguageOptions& GoogleSqlLanguageOptions() {
  static const googlesql::LanguageOptions* const kLanguageOptions = [] {
    auto* options = new googlesql::LanguageOptions();
    // Some syntax BigQuery accepts is gated behind a language feature that the default options
    // leave off, QUALIFY among it, so every released feature is turned on and known differences
    // from BigQuery are turned off below.
    options->EnableMaximumLanguageFeatures();
    // Some of those features add types and precision that BigQuery lacks rather than syntax, so
    // they are turned off again: BigQuery has no MAP, UUID or FLOAT32, and its timestamps hold
    // microseconds, not nanoseconds or picoseconds. BigQuery also rejects explicit casts between
    // arrays with different element types, including SAFE_CAST and arrays inside structs, and
    // comparing or ordering arrays, including arrays inside structs.
    for (const googlesql::LanguageFeature feature : {
             googlesql::FEATURE_MAP_TYPE,
             googlesql::FEATURE_UUID_TYPE,
             googlesql::FEATURE_TIMESTAMP_NANOS,
             googlesql::FEATURE_TIMESTAMP_PICOS,
             googlesql::FEATURE_CAST_DIFFERENT_ARRAY_TYPES,
             googlesql::FEATURE_ARRAY_EQUALITY,
             googlesql::FEATURE_ARRAY_ORDERING,
         }) {
      options->DisableLanguageFeature(feature);
    }
    // GoogleSQL released JSON_EXISTS functions before BigQuery documented them.
    options->DisableLanguageFeature(googlesql::FEATURE_JSON_EXISTS_FUNCTIONS);
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
  if (parts.size() == 1 && !parts.at(0).empty()) {
    return parts.at(0);
  }
  if (parts.size() == 2 && parts.at(0) == kSessionDataset && !parts.at(1).empty()) {
    return parts.at(1);
  }
  return std::nullopt;
}

std::vector<std::string> ResolveTablePath(absl::Span<const std::string> path,
                                          const std::string& default_project,
                                          const std::string& default_dataset,
                                          const TemporaryTables* temporary) {
  if (temporary != nullptr) {
    const std::vector<std::string> parts = SplitTablePath(path);
    const bool session = parts.size() == 2 && parts.at(0) == kSessionDataset;
    if (session || (parts.size() == 1 && temporary->names.contains(parts.at(0)))) {
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

absl::Status BigQueryCatalog::FindTableValuedFunction(
    const absl::Span<const std::string>& path, const googlesql::TableValuedFunction** function,
    const FindOptions& options) {
  if (path.size() != 1 || !absl::EqualsIgnoreCase(path.front(), "external_query")) {
    return googlesql::CatalogWrapper::FindTableValuedFunction(path, function, options);
  }
  if (external_query_ == nullptr) {
    const googlesql::FunctionArgumentType string(googlesql::types::StringType());
    external_query_ = std::make_unique<googlesql::TableValuedFunction>(
        std::vector<std::string>{"external_query"}, kBigQueryGroup,
        std::vector<googlesql::FunctionSignature>{
            googlesql::FunctionSignature(
                googlesql::ARG_KIND_RELATION,
                {
                    string,
                    string,
                    googlesql::FunctionArgumentType(googlesql::types::StringType(),
                                                    googlesql::FunctionArgumentType::OPTIONAL),
                },
                /*context_id=*/int64_t{0}),
        },
        googlesql::TableValuedFunctionOptions().set_compute_result_type_callback(
            [this](googlesql::Catalog* /*catalog*/, googlesql::TypeFactory* /*type_factory*/,
                   const googlesql::FunctionSignature& /*signature*/,
                   const std::vector<googlesql::TVFInputArgumentType>& arguments,
                   const googlesql::AnalyzerOptions& /*options*/) {
              return ExternalQuerySchema(arguments);
            }));
  }
  *function = external_query_.get();
  return absl::OkStatus();
}

// The columns of EXTERNAL_QUERY, which the database it queries describes when the query is
// analyzed. Its arguments must be literals, since the query is prepared then.
absl::StatusOr<std::shared_ptr<googlesql::TVFSignature>> BigQueryCatalog::ExternalQuerySchema(
    const std::vector<googlesql::TVFInputArgumentType>& arguments) {
  std::vector<std::string> literals;
  for (const auto& argument : arguments) {
    const auto scalar = argument.GetScalarArgType();
    if (!scalar.ok() || scalar->literal_value() == nullptr) {
      return absl::InvalidArgumentError(
          "The emulator does not support EXTERNAL_QUERY arguments that are not literals");
    }
    if (scalar->literal_value()->is_null()) {
      return absl::InvalidArgumentError("EXTERNAL_QUERY arguments must not be NULL");
    }
    literals.push_back(scalar->literal_value()->string_value());
  }
  const std::optional<std::string> key = ConnectionKey(literals.at(0), default_project_);
  if (!key.has_value()) {
    return absl::InvalidArgumentError("Invalid connection ID: " + literals.at(0));
  }
  const absl::StatusOr<FieldType> decimal_type =
      DecimalColumnType(literals.size() == 3 ? literals.at(2) : "{}");
  if (!decimal_type.ok()) {
    return decimal_type.status();
  }
  const absl::StatusOr<std::vector<FieldSchema>> columns =
      source_.DescribeExternalQuery(*key, literals.at(1), *decimal_type);
  if (!columns.ok()) {
    return columns.status();
  }
  std::vector<googlesql::TVFRelation::Column> relation;
  for (const FieldSchema& column : *columns) {
    const absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(column, type_factory_);
    if (!type.ok()) {
      return type.status();
    }
    relation.emplace_back(column.name, *type);
  }
  return googlesql::TVFSignature::Create(arguments, googlesql::TVFRelation(std::move(relation)));
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
      ResolveTablePath(path, default_project_, default_dataset_, temporary_);
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
