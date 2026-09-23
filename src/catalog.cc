#include "src/catalog.h"

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
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/type.h"
#include "src/field_schema.h"
#include "src/frontend.h"

namespace bigquery_emulator_duckdb {
namespace {

std::unique_ptr<googlesql::SimpleCatalog> MakeBuiltinCatalog(googlesql::TypeFactory* type_factory) {
  auto catalog = std::make_unique<googlesql::SimpleCatalog>("builtin", type_factory);
  const absl::Status status = catalog->AddBuiltinFunctionsAndTypes(
      googlesql::BuiltinFunctionOptions(GoogleSqlLanguageOptions()));
  if (!status.ok()) {
    throw std::runtime_error(status.ToString());
  }
  return catalog;
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

}  // namespace

std::vector<std::string> NormalizeTablePath(absl::Span<const std::string> path,
                                            const std::string& default_project,
                                            const std::string& default_dataset) {
  std::vector<std::string> parts;
  for (const std::string& element : path) {
    for (const absl::string_view part : absl::StrSplit(element, '.')) {
      parts.emplace_back(part);
    }
  }
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

BigQueryCatalog::BigQueryCatalog(TableSource& source, googlesql::TypeFactory* type_factory,
                                 std::string default_project, std::string default_dataset)
    : googlesql::CatalogWrapper(MakeBuiltinCatalog(type_factory)),
      source_(source),
      type_factory_(type_factory),
      default_project_(std::move(default_project)),
      default_dataset_(std::move(default_dataset)) {}

absl::Status BigQueryCatalog::FindTable(const absl::Span<const std::string>& path,
                                        const googlesql::Table** table,
                                        const FindOptions& /*options*/) {
  *table = nullptr;
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
