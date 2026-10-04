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

const googlesql::Type* ScalarType(FieldType type) {
  switch (type) {
    case FieldType::kString:
      return googlesql::types::StringType();
    case FieldType::kBytes:
      return googlesql::types::BytesType();
    case FieldType::kInteger:
      return googlesql::types::Int64Type();
    case FieldType::kFloat:
      return googlesql::types::DoubleType();
    case FieldType::kNumeric:
      return googlesql::types::NumericType();
    case FieldType::kBigNumeric:
      return googlesql::types::BigNumericType();
    case FieldType::kBoolean:
      return googlesql::types::BoolType();
    case FieldType::kTimestamp:
      return googlesql::types::TimestampType();
    case FieldType::kDate:
      return googlesql::types::DateType();
    case FieldType::kTime:
      return googlesql::types::TimeType();
    case FieldType::kDatetime:
      return googlesql::types::DatetimeType();
    case FieldType::kInterval:
      return googlesql::types::IntervalType();
    case FieldType::kGeography:
      return googlesql::types::GeographyType();
    case FieldType::kJson:
      return googlesql::types::JsonType();
    case FieldType::kRecord:
      break;
  }
  return nullptr;
}

absl::StatusOr<FieldType> BigQueryFieldType(const googlesql::Type* type) {
  switch (type->kind()) {
    case googlesql::TYPE_INT64:
      return FieldType::kInteger;
    case googlesql::TYPE_DOUBLE:
      return FieldType::kFloat;
    case googlesql::TYPE_BOOL:
      return FieldType::kBoolean;
    case googlesql::TYPE_STRING:
      return FieldType::kString;
    case googlesql::TYPE_BYTES:
      return FieldType::kBytes;
    case googlesql::TYPE_DATE:
      return FieldType::kDate;
    case googlesql::TYPE_TIME:
      return FieldType::kTime;
    case googlesql::TYPE_DATETIME:
      return FieldType::kDatetime;
    case googlesql::TYPE_TIMESTAMP:
      return FieldType::kTimestamp;
    case googlesql::TYPE_NUMERIC:
      return FieldType::kNumeric;
    case googlesql::TYPE_BIGNUMERIC:
      return FieldType::kBigNumeric;
    case googlesql::TYPE_JSON:
      return FieldType::kJson;
    case googlesql::TYPE_INTERVAL:
      return FieldType::kInterval;
    case googlesql::TYPE_GEOGRAPHY:
      return FieldType::kGeography;
    case googlesql::TYPE_STRUCT:
      return FieldType::kRecord;
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
      // A BIGNUM keeps no precision or scale to round to.
      if (type->IsBigNumericType() || numeric.is_max_precision() || numeric.precision() > 38) {
        return std::nullopt;
      }
      return "DECIMAL(" + std::to_string(numeric.precision()) + "," +
             std::to_string(numeric.scale()) + ")";
    }
    const int children = type->IsArray()    ? 1
                         : type->IsStruct() ? type->AsStruct()->num_fields()
                                            : -1;
    if (!parameters->IsTopLevelEmpty() ||
        std::cmp_not_equal(parameters->num_children(), children)) {
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
      // The integer number of units of 10^-38; see src/bignumeric.h.
      return "BIGNUM";
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
  if (field.type == FieldType::kRecord) {
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
    type = ScalarType(field.type);
  }
  if (field.mode == FieldMode::kRepeated) {
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
  if (type->IsArray()) {
    field.mode = FieldMode::kRepeated;
    type = type->AsArray()->element_type();
    if (type->IsArray()) {
      return absl::InvalidArgumentError("Unsupported result type: an array of arrays");
    }
  }
  absl::StatusOr<FieldType> field_type = BigQueryFieldType(type);
  if (!field_type.ok()) {
    return field_type.status();
  }
  field.type = *field_type;
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
