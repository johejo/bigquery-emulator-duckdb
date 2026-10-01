#include "src/type_mapping.h"

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_join.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/type_parameters.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {
namespace {

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

std::optional<std::string> MapToDuckDb(const googlesql::Type* type,
                                       const googlesql::TypeParameters* parameters,
                                       bool geography_as_text) {
  if (parameters != nullptr && (parameters->IsEmpty() || parameters->IsStringTypeParameters())) {
    parameters = nullptr;
  }
  if (parameters != nullptr) {
    if (parameters->IsNumericTypeParameters()) {
      const auto& numeric = parameters->numeric_type_parameters();
      if (numeric.is_max_precision() || numeric.precision() > 38) {
        return std::nullopt;
      }
      return "DECIMAL(" + std::to_string(numeric.precision()) + "," +
             std::to_string(numeric.scale()) + ")";
    }
    const int children = type->IsArray()    ? 1
                         : type->IsStruct() ? type->AsStruct()->num_fields()
                                            : -1;
    if (!parameters->IsTopLevelEmpty() || parameters->num_children() != children) {
      return std::nullopt;
    }
  }
  const auto child = [parameters](int i) {
    return parameters == nullptr ? nullptr : &parameters->child(i);
  };
  switch (type->kind()) {
    case googlesql::TYPE_INT64:
      return "BIGINT";
    case googlesql::TYPE_DOUBLE:
      return "DOUBLE";
    case googlesql::TYPE_BOOL:
      return "BOOLEAN";
    case googlesql::TYPE_STRING:
      return "VARCHAR";
    case googlesql::TYPE_BYTES:
      return "BLOB";
    case googlesql::TYPE_DATE:
      return "DATE";
    case googlesql::TYPE_TIMESTAMP:
      return "TIMESTAMPTZ";
    case googlesql::TYPE_DATETIME:
      return "TIMESTAMP";
    case googlesql::TYPE_TIME:
      return "TIME";
    case googlesql::TYPE_NUMERIC:
      return "DECIMAL(38,9)";
    case googlesql::TYPE_BIGNUMERIC:
      // Narrower than BIGNUMERIC, but the widest DuckDB decimal; out of range values fail.
      return "DECIMAL(38,19)";
    case googlesql::TYPE_JSON:
      return "JSON";
    case googlesql::TYPE_GEOGRAPHY:
      // Stored as text; queries over it are not translated.
      return geography_as_text ? std::optional<std::string>("VARCHAR") : std::nullopt;
    case googlesql::TYPE_ARRAY: {
      const auto element =
          MapToDuckDb(type->AsArray()->element_type(), child(0), geography_as_text);
      return element ? std::optional<std::string>(*element + "[]") : std::nullopt;
    }
    case googlesql::TYPE_STRUCT: {
      // DuckDB structs need distinct field names, which anonymous BigQuery fields lack.
      std::set<std::string> names;
      std::vector<std::string> fields;
      for (int i = 0; i < type->AsStruct()->num_fields(); ++i) {
        const auto& field = type->AsStruct()->field(i);
        const auto field_type = MapToDuckDb(field.type, child(i), geography_as_text);
        if (!field_type || field.name.empty() || !names.insert(ToLowerAscii(field.name)).second) {
          return std::nullopt;
        }
        fields.push_back(QuoteIdentifier(field.name) + " " + *field_type);
      }
      return fields.empty()
                 ? std::nullopt
                 : std::optional<std::string>("STRUCT(" + absl::StrJoin(fields, ", ") + ")");
    }
    default:
      return std::nullopt;
  }
}

}  // namespace

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

std::optional<std::string> DuckDbType(const googlesql::Type* type,
                                      const googlesql::TypeParameters* parameters) {
  return MapToDuckDb(type, parameters, /*geography_as_text=*/false);
}

absl::StatusOr<std::string> DuckDbColumnType(const FieldSchema& field) {
  googlesql::TypeFactory type_factory;
  absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(field, &type_factory);
  if (!type.ok()) {
    return type.status();
  }
  std::optional<std::string> duckdb_type = MapToDuckDb(*type, nullptr, /*geography_as_text=*/true);
  if (!duckdb_type.has_value()) {
    return absl::InvalidArgumentError("Unsupported field type: " +
                                      (*type)->TypeName(googlesql::PRODUCT_EXTERNAL));
  }
  return *std::move(duckdb_type);
}

}  // namespace bigquery_emulator_duckdb
