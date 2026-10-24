#include "src/type_mapping.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_join.h"
#include "duckdb.h"
#include "googlesql/public/type.h"
#include "googlesql/public/type_parameters.pb.h"
#include "googlesql/public/types/type_parameters.h"
#include "src/bignumeric.h"
#include "src/duckdb_handle.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/googlesql_geography_duckdb/codec.h"

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
    case FieldType::kRange:
      break;
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
    case googlesql::TYPE_RANGE:
      return FieldType::kRange;
    case googlesql::TYPE_GEOGRAPHY:
      return FieldType::kGeography;
    case googlesql::TYPE_STRUCT:
      // A RECORD has at least one field.
      if (type->AsStruct()->num_fields() == 0) {
        return absl::InvalidArgumentError("Unsupported result type: STRUCT<>");
      }
      return FieldType::kRecord;
    default:
      return absl::InvalidArgumentError("Unsupported result type: " +
                                        type->ShortTypeName(googlesql::PRODUCT_EXTERNAL));
  }
}

// What a DuckDB type is for: a query value, a stored column, or a query parameter, which is a
// query value whose type a TableFieldSchema describes. A resolved column definition can still
// have anonymous STRUCT fields, which take positional names when stored.
enum class TypeUse : std::uint8_t { kQuery, kColumn, kParameter, kDefinition };

std::optional<std::string> MapToDuckDb(const googlesql::Type* type,
                                       const googlesql::TypeParameters* parameters, TypeUse use) {
  if (parameters != nullptr && (parameters->IsEmpty() || parameters->IsStringTypeParameters())) {
    parameters = nullptr;
  }
  if (parameters != nullptr) {
    if (parameters->IsNumericTypeParameters()) {
      const auto& numeric = parameters->numeric_type_parameters();
      if (numeric.is_max_precision()) {
        return std::nullopt;
      }
      if (type->IsBigNumericType()) {
        return BigNumericTypeName(numeric.precision(), numeric.scale());
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
    case googlesql::TYPE_GRAPH_ELEMENT: {
      if (use != TypeUse::kQuery) {
        return std::nullopt;
      }
      std::vector<std::string> properties;
      for (const auto& property : type->AsGraphElement()->property_types()) {
        const auto mapped = MapToDuckDb(property.value_type, nullptr, use);
        if (!mapped) {
          return std::nullopt;
        }
        properties.push_back(QuoteIdentifier(property.name) + " " + *mapped);
      }
      if (properties.empty()) {
        properties.emplace_back("\"$empty\" BOOLEAN");
      }
      const std::string endpoints = type->AsGraphElement()->IsEdge()
                                        ? R"(, "$source_id" VARCHAR, "$destination_id" VARCHAR)"
                                        : "";
      return R"(STRUCT("$id" VARCHAR)" + endpoints +
             R"(, "$labels" VARCHAR[], "$properties" STRUCT()" + Join(properties, ", ") + "))";
    }
    case googlesql::TYPE_GRAPH_PATH: {
      if (use != TypeUse::kQuery) {
        return std::nullopt;
      }
      const auto nodes = MapToDuckDb(type->AsGraphPath()->node_type(), nullptr, use);
      const auto edges = MapToDuckDb(type->AsGraphPath()->edge_type(), nullptr, use);
      return nodes && edges ? std::optional<std::string>("STRUCT(\"$nodes\" " + *nodes +
                                                         "[], \"$edges\" " + *edges + "[])")
                            : std::nullopt;
    }
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
      return use == TypeUse::kColumn || use == TypeUse::kDefinition ? kBigNumericStorageType
                                                                    : "BIGNUM";
    case googlesql::TYPE_JSON:
      return "JSON";
    case googlesql::TYPE_INTERVAL:
      return "INTERVAL";
    case googlesql::TYPE_RANGE: {
      const auto element = MapToDuckDb(type->AsRange()->element_type(), nullptr, use);
      return element ? std::optional<std::string>(
                           std::format("STRUCT({} {}, {} {})", QuoteIdentifier(kRangeStart),
                                       *element, QuoteIdentifier(kRangeEnd), *element))
                     : std::nullopt;
    }
    case googlesql::TYPE_GEOGRAPHY:
      // See src/googlesql_geography_duckdb/codec.h.
      return use == TypeUse::kColumn || use == TypeUse::kDefinition
                 ? geography_duckdb::kGeographyStorageType
                 : "BLOB";
    case googlesql::TYPE_ARRAY: {
      const auto element = MapToDuckDb(type->AsArray()->element_type(), child(0), use);
      return element ? std::optional<std::string>(*element + "[]") : std::nullopt;
    }
    case googlesql::TYPE_STRUCT: {
      const auto names = DuckDbStructFieldNames(type->AsStruct());
      std::vector<std::string> fields;
      for (int i = 0; i < type->AsStruct()->num_fields(); ++i) {
        const auto& field = type->AsStruct()->field(i);
        const auto field_type = MapToDuckDb(field.type, child(i), use);
        // Metadata and parameters already have their final field names. Query values and
        // resolved SQL definitions can still need positional names.
        if (!field_type || ((use == TypeUse::kColumn || use == TypeUse::kParameter) &&
                            names.at(i) != field.name)) {
          return std::nullopt;
        }
        fields.push_back(QuoteIdentifier(names.at(i)) + " " + *field_type);
      }
      // DuckDB has no empty STRUCT, and BigQuery stores none.
      if (fields.empty()) {
        return use == TypeUse::kQuery ? std::optional<std::string>(std::format(
                                            "STRUCT({} BOOLEAN)", QuoteIdentifier(kEmptyStruct)))
                                      : std::nullopt;
      }
      return "STRUCT(" + absl::StrJoin(fields, ", ") + ")";
    }
    default:
      return std::nullopt;
  }
}

absl::StatusOr<std::string> DuckDbFieldType(const FieldSchema& field, TypeUse use) {
  googlesql::TypeFactory type_factory;
  absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(field, &type_factory);
  if (!type.ok()) {
    return type.status();
  }
  absl::StatusOr<googlesql::TypeParameters> parameters = FieldTypeParameters(field);
  if (!parameters.ok()) {
    return parameters.status();
  }
  std::optional<std::string> duckdb_type = MapToDuckDb(*type, &*parameters, use);
  if (duckdb_type.has_value()) {
    return *std::move(duckdb_type);
  }
  return absl::InvalidArgumentError("Unsupported field type: " +
                                    (*type)->TypeName(googlesql::PRODUCT_EXTERNAL));
}

// Maps a DuckDB type to the BigQuery type of a TableFieldSchema.
FieldType ToBigQueryType(duckdb_logical_type type) {
  if (IsRange(type)) {
    return FieldType::kRange;
  }
  switch (duckdb_get_type_id(type)) {
    case DUCKDB_TYPE_BOOLEAN:
      return FieldType::kBoolean;
    case DUCKDB_TYPE_TINYINT:
    case DUCKDB_TYPE_SMALLINT:
    case DUCKDB_TYPE_INTEGER:
    case DUCKDB_TYPE_BIGINT:
    case DUCKDB_TYPE_UTINYINT:
    case DUCKDB_TYPE_USMALLINT:
    case DUCKDB_TYPE_UINTEGER:
      return FieldType::kInteger;
    case DUCKDB_TYPE_UBIGINT:
    case DUCKDB_TYPE_HUGEINT:
    case DUCKDB_TYPE_UHUGEINT:
    case DUCKDB_TYPE_BIGNUM:
      return FieldType::kBigNumeric;
    case DUCKDB_TYPE_FLOAT:
    case DUCKDB_TYPE_DOUBLE:
      return FieldType::kFloat;
    case DUCKDB_TYPE_DECIMAL:
      return duckdb_decimal_width(type) - duckdb_decimal_scale(type) <= 29 &&
                     duckdb_decimal_scale(type) <= 9
                 ? FieldType::kNumeric
                 : FieldType::kBigNumeric;
    case DUCKDB_TYPE_VARCHAR:
    case DUCKDB_TYPE_UUID:
      return FieldType::kString;
    case DUCKDB_TYPE_BLOB:
      return FieldType::kBytes;
    case DUCKDB_TYPE_DATE:
      return FieldType::kDate;
    case DUCKDB_TYPE_TIME:
    case DUCKDB_TYPE_TIME_TZ:
      return FieldType::kTime;
    // BigQuery TIMESTAMP is an absolute instant, which is DuckDB's TIMESTAMP WITH TIME ZONE.
    // BigQuery DATETIME is a civil time, which is DuckDB's plain TIMESTAMP.
    case DUCKDB_TYPE_TIMESTAMP_TZ:
      return FieldType::kTimestamp;
    case DUCKDB_TYPE_TIMESTAMP:
    case DUCKDB_TYPE_TIMESTAMP_S:
    case DUCKDB_TYPE_TIMESTAMP_MS:
    case DUCKDB_TYPE_TIMESTAMP_NS:
      return FieldType::kDatetime;
    case DUCKDB_TYPE_INTERVAL:
      return FieldType::kInterval;
    case DUCKDB_TYPE_STRUCT:
      return FieldType::kRecord;
    case DUCKDB_TYPE_LIST:
    case DUCKDB_TYPE_ARRAY:
      return ToBigQueryType(ElementType(type).get());
    default:
      return FieldType::kString;
  }
}

std::optional<std::string> ReadNumericStorage(const std::string& sql, duckdb_logical_type type,
                                              int depth) {
  switch (duckdb_get_type_id(type)) {
    case DUCKDB_TYPE_UBIGINT:
    case DUCKDB_TYPE_HUGEINT:
    case DUCKDB_TYPE_UHUGEINT:
      return "CAST(bq_bignumeric_from_string(CAST(" + sql + " AS VARCHAR), false) AS BIGNUM)";
    case DUCKDB_TYPE_DECIMAL:
      if (ToBigQueryType(type) == FieldType::kBigNumeric) {
        return "CAST(bq_bignumeric_from_string(CAST(" + sql + " AS VARCHAR), false) AS BIGNUM)";
      }
      return std::nullopt;
    case DUCKDB_TYPE_LIST:
    case DUCKDB_TYPE_ARRAY: {
      const std::string element = "_bq_numeric_" + std::to_string(depth);
      const auto converted = ReadNumericStorage(element, ElementType(type).get(), depth + 1);
      if (!converted) {
        return std::nullopt;
      }
      return "list_transform(" + sql + ", lambda " + element + ": " + *converted + ")";
    }
    case DUCKDB_TYPE_STRUCT: {
      std::vector<std::string> fields;
      bool changed = false;
      for (idx_t i = 0; i < duckdb_struct_type_child_count(type); ++i) {
        DuckString const name(duckdb_struct_type_child_name(type, i));
        LogicalType const child(duckdb_struct_type_child_type(type, i));
        const std::string field = QuoteIdentifier(name.get());
        const std::string access = std::format("({}).{}", sql, field);
        const auto converted = ReadNumericStorage(access, child.get(), depth + 1);
        changed |= converted.has_value();
        fields.push_back(field + " := " + converted.value_or(access));
      }
      if (!changed) {
        return std::nullopt;
      }
      return "CASE WHEN " + sql + " IS NULL THEN NULL ELSE struct_pack(" + Join(fields, ", ") +
             ") END";
    }
    default:
      return std::nullopt;
  }
}

}  // namespace

absl::StatusOr<std::string> GoogleSqlTypeName(const FieldSchema& field,
                                              googlesql::TypeFactory* type_factory) {
  absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(field, type_factory);
  if (!type.ok()) {
    return type.status();
  }
  return (*type)->TypeName(googlesql::PRODUCT_EXTERNAL);
}

// The type parameters of a column that `field` describes: the precision and scale of NUMERIC and
// BIGNUMERIC, at any depth, within the ranges DDL allows and failing with GoogleSQL's messages
// outside them. Storage leaves lengths out; default validation includes them.
absl::StatusOr<googlesql::TypeParameters> FieldTypeParameters(const FieldSchema& field,
                                                              bool include_lengths) {
  googlesql::TypeParameters parameters;
  if (field.type == FieldType::kRecord) {
    std::vector<googlesql::TypeParameters> children;
    for (const FieldSchema& child : field.fields) {
      absl::StatusOr<googlesql::TypeParameters> child_parameters =
          FieldTypeParameters(child, include_lengths);
      if (!child_parameters.ok()) {
        return child_parameters.status();
      }
      children.push_back(*std::move(child_parameters));
    }
    parameters = googlesql::TypeParameters::MakeTypeParametersWithChildList(std::move(children));
  } else if (include_lengths && field.max_length.has_value()) {
    googlesql::StringTypeParametersProto proto;
    proto.set_max_length(*field.max_length);
    auto string_parameters = googlesql::TypeParameters::MakeStringTypeParameters(proto);
    if (!string_parameters.ok()) {
      return string_parameters.status();
    }
    parameters = *std::move(string_parameters);
  } else if (field.precision.has_value() || field.scale.has_value()) {
    const bool numeric = field.type == FieldType::kNumeric;
    if (!numeric && field.type != FieldType::kBigNumeric) {
      return absl::InvalidArgumentError(
          std::format("Field {} of type {} cannot have a precision or scale", field.name,
                      field.type == FieldType::kRange
                          ? "RANGE"
                          : ScalarType(field.type)->TypeName(googlesql::PRODUCT_EXTERNAL)));
    }
    if (!field.precision.has_value()) {
      return absl::InvalidArgumentError("Field " + field.name + " has a scale but no precision");
    }
    const std::string_view name = numeric ? "NUMERIC" : "BIGNUMERIC";
    const int64_t scale = field.scale.value_or(0);
    const int64_t max_scale = numeric ? 9 : 38;
    if (scale < 0 || scale > max_scale) {
      return absl::InvalidArgumentError(
          std::format("In {}(P, S), S must be between 0 and {}", name, max_scale));
    }
    const int64_t max_precision = (numeric ? 29 : 38) + scale;
    if (*field.precision < std::max<int64_t>(1, scale) || *field.precision > max_precision) {
      return absl::InvalidArgumentError(std::format("In {}(P, {}), P must be between {} and {}",
                                                    name, scale, std::max<int64_t>(1, scale),
                                                    max_precision));
    }
    googlesql::NumericTypeParametersProto proto;
    proto.set_precision(*field.precision);
    proto.set_scale(scale);
    absl::StatusOr<googlesql::TypeParameters> numeric_parameters =
        googlesql::TypeParameters::MakeNumericTypeParameters(proto);
    if (!numeric_parameters.ok()) {
      return numeric_parameters.status();
    }
    parameters = *std::move(numeric_parameters);
  }
  if (field.mode == FieldMode::kRepeated) {
    return googlesql::TypeParameters::MakeTypeParametersWithChildList({std::move(parameters)});
  }
  return parameters;
}

std::vector<std::string> DuckDbStructFieldNames(const googlesql::StructType* type) {
  std::set<std::string> seen;
  std::vector<std::string> names;
  bool positional = false;
  for (const auto& field : type->fields()) {
    // A name starting with $ could pass for a RANGE's; BigQuery's column names cannot have one.
    positional = positional || field.name.empty() || field.name.starts_with('$') ||
                 !seen.insert(ToLowerAscii(field.name)).second;
    names.push_back(field.name);
  }
  if (positional) {
    for (size_t i = 0; i < names.size(); ++i) {
      names.at(i) = "_field_" + std::to_string(i + 1);
    }
  }
  return names;
}

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
  } else if (field.type == FieldType::kRange) {
    const googlesql::Type* element =
        field.range_element_type.has_value() ? ScalarType(*field.range_element_type) : nullptr;
    if (element == nullptr) {
      return absl::InvalidArgumentError("Field " + field.name +
                                        " of type RANGE has no element type");
    }
    if (absl::Status status = type_factory->MakeRangeType(element, &type); !status.ok()) {
      return status;
    }
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
  if (type->IsRange()) {
    absl::StatusOr<FieldType> element = BigQueryFieldType(type->AsRange()->element_type());
    if (!element.ok()) {
      return element.status();
    }
    field.range_element_type = *element;
  }
  if (type->IsStruct()) {
    const std::vector<googlesql::StructField>& struct_fields = type->AsStruct()->fields();
    for (size_t i = 0; i < struct_fields.size(); ++i) {
      const std::string& child_name = struct_fields.at(i).name;
      absl::StatusOr<FieldSchema> child =
          BigQueryFieldSchema(child_name.empty() ? "_field_" + std::to_string(i + 1) : child_name,
                              struct_fields.at(i).type);
      if (!child.ok()) {
        return child.status();
      }
      field.fields.push_back(*std::move(child));
    }
  }
  return field;
}

bool HasInterval(const googlesql::Type* type) {
  if (type->IsInterval()) {
    return true;
  }
  if (type->IsArray()) {
    return HasInterval(type->AsArray()->element_type());
  }
  if (type->IsStruct()) {
    return std::ranges::any_of(type->AsStruct()->fields(),
                               [](const auto& field) { return HasInterval(field.type); });
  }
  return false;
}

bool HasGeography(const googlesql::Type* type) {
  if (type->IsGeography()) {
    return true;
  }
  if (type->IsArray()) {
    return HasGeography(type->AsArray()->element_type());
  }
  if (type->IsStruct()) {
    return std::ranges::any_of(type->AsStruct()->fields(),
                               [](const auto& field) { return HasGeography(field.type); });
  }
  return false;
}

bool IsRange(duckdb_logical_type type) {
  if (duckdb_get_type_id(type) != DUCKDB_TYPE_STRUCT || duckdb_struct_type_child_count(type) != 2) {
    return false;
  }
  DuckString const start(duckdb_struct_type_child_name(type, 0));
  DuckString const end(duckdb_struct_type_child_name(type, 1));
  return start.get() == kRangeStart && end.get() == kRangeEnd;
}

bool IsEmptyStruct(duckdb_logical_type type) {
  if (duckdb_get_type_id(type) != DUCKDB_TYPE_STRUCT || duckdb_struct_type_child_count(type) != 1) {
    return false;
  }
  DuckString const name(duckdb_struct_type_child_name(type, 0));
  return name.get() == kEmptyStruct;
}

bool HasEmptyStruct(const googlesql::Type* type) {
  if (type->IsArray()) {
    return HasEmptyStruct(type->AsArray()->element_type());
  }
  if (type->IsStruct()) {
    return type->AsStruct()->num_fields() == 0 ||
           std::ranges::any_of(type->AsStruct()->fields(),
                               [](const auto& field) { return HasEmptyStruct(field.type); });
  }
  return false;
}

bool HasRange(const googlesql::Type* type) {
  if (type->IsRange()) {
    return true;
  }
  if (type->IsArray()) {
    return HasRange(type->AsArray()->element_type());
  }
  if (type->IsStruct()) {
    return std::ranges::any_of(type->AsStruct()->fields(),
                               [](const auto& field) { return HasRange(field.type); });
  }
  return false;
}

std::string DuckDbRange(std::string_view start, std::string_view end, std::string_view raise) {
  return std::format(
      "CASE WHEN {0} >= {1} THEN {2} ELSE struct_pack({3} := coalesce({0}, '-infinity'), {4} := "
      "coalesce({1}, 'infinity')) END",
      start, end,
      raise.empty() ? "error(" + QuoteLiteral(kRangeOrderError) + ")" : std::string(raise),
      QuoteIdentifier(kRangeStart), QuoteIdentifier(kRangeEnd));
}

std::optional<std::string> DuckDbType(const googlesql::Type* type,
                                      const googlesql::TypeParameters* parameters) {
  return MapToDuckDb(type, parameters, TypeUse::kQuery);
}

bool HasGraph(const googlesql::Type* type) {
  if (type->IsArray()) {
    return HasGraph(type->AsArray()->element_type());
  }
  if (type->IsStruct()) {
    return std::ranges::any_of(type->AsStruct()->fields(),
                               [](const auto& field) { return HasGraph(field.type); });
  }
  return type->IsGraphElement() || type->IsGraphPath();
}

std::optional<std::string> DuckDbColumnType(const googlesql::Type* type,
                                            const googlesql::TypeParameters* parameters) {
  return MapToDuckDb(type, parameters, TypeUse::kDefinition);
}

absl::StatusOr<std::string> DuckDbColumnType(const FieldSchema& field) {
  return DuckDbFieldType(field, TypeUse::kColumn);
}

absl::StatusOr<std::string> DuckDbParameterType(const FieldSchema& field) {
  return DuckDbFieldType(field, TypeUse::kParameter);
}

bool IsListLike(duckdb_logical_type type) {
  return duckdb_get_type_id(type) == DUCKDB_TYPE_LIST ||
         duckdb_get_type_id(type) == DUCKDB_TYPE_ARRAY;
}

LogicalType ElementType(duckdb_logical_type type) {
  return LogicalType(duckdb_get_type_id(type) == DUCKDB_TYPE_ARRAY
                         ? duckdb_array_type_child_type(type)
                         : duckdb_list_type_child_type(type));
}

FieldSchema ResultFieldSchema(const std::string& name, duckdb_logical_type type) {
  FieldSchema field;
  field.name = name;
  field.type = ToBigQueryType(type);
  field.mode = IsListLike(type) ? FieldMode::kRepeated : FieldMode::kNullable;
  LogicalType const element(IsListLike(type) ? ElementType(type).release() : nullptr);
  duckdb_logical_type scalar_type = element.get() != nullptr ? element.get() : type;
  if (IsRange(scalar_type)) {
    LogicalType const bound(duckdb_struct_type_child_type(scalar_type, 0));
    field.range_element_type = ToBigQueryType(bound.get());
  } else if (duckdb_get_type_id(scalar_type) == DUCKDB_TYPE_STRUCT) {
    for (idx_t i = 0; i < duckdb_struct_type_child_count(scalar_type); ++i) {
      DuckString const child_name(duckdb_struct_type_child_name(scalar_type, i));
      LogicalType const child_type(duckdb_struct_type_child_type(scalar_type, i));
      field.fields.push_back(ResultFieldSchema(child_name.get(), child_type.get()));
    }
  }
  return field;
}

std::string UnsupportedDuckDbType(duckdb_logical_type type) {
  switch (duckdb_get_type_id(type)) {
    case DUCKDB_TYPE_BIGNUM: {
      DuckString const alias(duckdb_logical_type_get_alias(type));
      return alias.get() != nullptr && std::string_view(alias.get()).starts_with("bq_bignumeric_")
                 ? ""
                 : "BIGNUM";
    }
    case DUCKDB_TYPE_LIST:
    case DUCKDB_TYPE_ARRAY:
      return UnsupportedDuckDbType(ElementType(type).get());
    case DUCKDB_TYPE_STRUCT:
      for (idx_t i = 0; i < duckdb_struct_type_child_count(type); ++i) {
        LogicalType const child(duckdb_struct_type_child_type(type, i));
        if (std::string child_type = UnsupportedDuckDbType(child.get()); !child_type.empty()) {
          return child_type;
        }
      }
      return "";
    default:
      return "";
  }
}

std::optional<std::string> ReadNumericStorage(const std::string& sql, duckdb_logical_type type) {
  return ReadNumericStorage(sql, type, 0);
}

}  // namespace bigquery_emulator_duckdb
