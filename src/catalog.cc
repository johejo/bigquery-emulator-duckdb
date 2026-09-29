#include "src/catalog.h"

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

absl::StatusOr<const googlesql::Type*> ScalarType(const std::string& type) {
  if (type == "INTEGER" || type == "INT64") {
    return googlesql::types::Int64Type();
  }
  if (type == "FLOAT" || type == "FLOAT64") {
    return googlesql::types::DoubleType();
  }
  if (type == "BOOLEAN" || type == "BOOL") {
    return googlesql::types::BoolType();
  }
  if (type == "STRING") {
    return googlesql::types::StringType();
  }
  if (type == "BYTES") {
    return googlesql::types::BytesType();
  }
  if (type == "DATE") {
    return googlesql::types::DateType();
  }
  if (type == "TIME") {
    return googlesql::types::TimeType();
  }
  if (type == "DATETIME") {
    return googlesql::types::DatetimeType();
  }
  if (type == "TIMESTAMP") {
    return googlesql::types::TimestampType();
  }
  if (type == "NUMERIC") {
    return googlesql::types::NumericType();
  }
  if (type == "BIGNUMERIC") {
    return googlesql::types::BigNumericType();
  }
  if (type == "JSON") {
    return googlesql::types::JsonType();
  }
  if (type == "INTERVAL") {
    return googlesql::types::IntervalType();
  }
  if (type == "GEOGRAPHY") {
    return googlesql::types::GeographyType();
  }
  return absl::InvalidArgumentError("Unsupported field type: " + type);
}

absl::StatusOr<std::string> BigQueryTypeName(const googlesql::Type* type) {
  switch (type->kind()) {
    case googlesql::TYPE_INT64:
      return "INTEGER";
    case googlesql::TYPE_DOUBLE:
      return "FLOAT";
    case googlesql::TYPE_BOOL:
      return "BOOLEAN";
    case googlesql::TYPE_STRING:
      return "STRING";
    case googlesql::TYPE_BYTES:
      return "BYTES";
    case googlesql::TYPE_DATE:
      return "DATE";
    case googlesql::TYPE_TIME:
      return "TIME";
    case googlesql::TYPE_DATETIME:
      return "DATETIME";
    case googlesql::TYPE_TIMESTAMP:
      return "TIMESTAMP";
    case googlesql::TYPE_NUMERIC:
      return "NUMERIC";
    case googlesql::TYPE_BIGNUMERIC:
      return "BIGNUMERIC";
    case googlesql::TYPE_JSON:
      return "JSON";
    case googlesql::TYPE_INTERVAL:
      return "INTERVAL";
    case googlesql::TYPE_GEOGRAPHY:
      return "GEOGRAPHY";
    case googlesql::TYPE_STRUCT:
      return "RECORD";
    default:
      return absl::InvalidArgumentError("Unsupported result type: " +
                                        type->ShortTypeName(googlesql::PRODUCT_EXTERNAL));
  }
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
    // BigQuery has EDIT_DISTANCE over BYTES, which GoogleSQL still marks in development.
    options->EnableLanguageFeature(googlesql::FEATURE_ENABLE_EDIT_DISTANCE_BYTES);
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
      parts.emplace_back(part);
    }
  }
  // Domain-scoped project IDs contain dots before the colon. Those dots belong to the
  // project, even when GoogleSQL passes the entire backtick-quoted path as one element.
  for (size_t i = 0; i < parts.size(); ++i) {
    if (parts[i].find(':') != std::string::npos) {
      if (i > 0) {
        const auto project_end =
            std::next(parts.begin(), static_cast<std::vector<std::string>::difference_type>(i + 1));
        const std::string project = absl::StrJoin(parts.begin(), project_end, ".");
        parts.erase(parts.begin(), project_end);
        parts.insert(parts.begin(), project);
      }
      break;
    }
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
  for (const std::string& part : parts) {
    if (part.empty()) {
      return {};
    }
  }
  return parts;
}

absl::StatusOr<const googlesql::Type*> GoogleSqlType(const FieldSchema& field,
                                                     googlesql::TypeFactory* type_factory) {
  const googlesql::Type* type = nullptr;
  if (field.type == "RECORD" || field.type == "STRUCT") {
    std::vector<googlesql::StructField> struct_fields;
    for (const FieldSchema& child : field.fields) {
      absl::StatusOr<const googlesql::Type*> child_type = GoogleSqlType(child, type_factory);
      if (!child_type.ok()) {
        return child_type.status();
      }
      struct_fields.emplace_back(child.name, *child_type);
    }
    const googlesql::StructType* struct_type = nullptr;
    if (absl::Status status = type_factory->MakeStructType(struct_fields, &struct_type);
        !status.ok()) {
      return status;
    }
    type = struct_type;
  } else {
    absl::StatusOr<const googlesql::Type*> scalar_type = ScalarType(field.type);
    if (!scalar_type.ok()) {
      return scalar_type.status();
    }
    type = *scalar_type;
  }
  if (field.mode == "REPEATED") {
    absl::StatusOr<const googlesql::ArrayType*> array_type = type_factory->MakeArrayType(type);
    if (!array_type.ok()) {
      return array_type.status();
    }
    type = *array_type;
  }
  return type;
}

absl::StatusOr<FieldSchema> BigQueryFieldSchema(const std::string& name,
                                                const googlesql::Type* type) {
  FieldSchema field;
  field.name = name;
  field.mode = "NULLABLE";
  if (type->IsArray()) {
    field.mode = "REPEATED";
    type = type->AsArray()->element_type();
    if (type->IsArray()) {
      return absl::InvalidArgumentError("Unsupported result type: an array of arrays");
    }
  }
  absl::StatusOr<std::string> type_name = BigQueryTypeName(type);
  if (!type_name.ok()) {
    return type_name.status();
  }
  field.type = *std::move(type_name);
  if (type->IsStruct()) {
    const std::vector<googlesql::StructField>& struct_fields = type->AsStruct()->fields();
    for (size_t i = 0; i < struct_fields.size(); ++i) {
      const std::string& child_name = struct_fields[i].name;
      absl::StatusOr<FieldSchema> child =
          BigQueryFieldSchema(child_name.empty() ? "_field_" + std::to_string(i + 1) : child_name,
                              struct_fields[i].type);
      if (!child.ok()) {
        return child.status();
      }
      field.fields.push_back(*std::move(child));
    }
  }
  return field;
}

BigQueryCatalog::BigQueryCatalog(TableSource& source, googlesql::TypeFactory* type_factory,
                                 std::string default_project, std::string default_dataset)
    : googlesql::CatalogWrapper(BuiltinCatalog()),
      source_(source),
      type_factory_(type_factory),
      default_project_(std::move(default_project)),
      default_dataset_(std::move(default_dataset)) {}

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
      NormalizeTablePath(path, default_project_, default_dataset_);
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
  auto simple_table = std::make_unique<googlesql::SimpleTable>(normalized[2]);
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
