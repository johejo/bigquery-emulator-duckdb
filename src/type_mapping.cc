#include "src/type_mapping.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_join.h"
#include "googlesql/public/type.h"
#include "googlesql/public/type_parameters.pb.h"
#include "googlesql/public/types/annotation.h"
#include "googlesql/public/types/simple_value.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/types/type_parameters.h"
#include "src/bignumeric.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/googlesql_geography_duckdb/codec.h"
#include "src/timestamp_precision.h"

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
                                       const googlesql::TypeParameters* parameters, TypeUse use,
                                       TimestampPrecision precision) {
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
        const auto mapped = MapToDuckDb(property.value_type, nullptr, use, precision);
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
      const auto nodes = MapToDuckDb(type->AsGraphPath()->node_type(), nullptr, use, precision);
      const auto edges = MapToDuckDb(type->AsGraphPath()->edge_type(), nullptr, use, precision);
      return nodes && edges ? std::optional<std::string>("STRUCT(\"$nodes\" " + *nodes +
                                                         "[], \"$edges\" " + *edges + "[])")
                            : std::nullopt;
    }
    case googlesql::TYPE_BYTES:
      return "BLOB";
    case googlesql::TYPE_DATE:
      return "DATE";
    case googlesql::TYPE_TIMESTAMP:
      return precision == TimestampPrecision::kNanoseconds ? kTimestampNanosType : "TIMESTAMPTZ";
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
      const auto element = MapToDuckDb(type->AsRange()->element_type(), nullptr, use,
                                       TimestampPrecision::kMicroseconds);
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
      const auto element = MapToDuckDb(type->AsArray()->element_type(), child(0), use, precision);
      return element ? std::optional<std::string>(*element + "[]") : std::nullopt;
    }
    case googlesql::TYPE_STRUCT: {
      const auto names = DuckDbStructFieldNames(type->AsStruct());
      std::vector<std::string> fields;
      for (int i = 0; i < type->AsStruct()->num_fields(); ++i) {
        const auto& field = type->AsStruct()->field(i);
        const auto field_type = MapToDuckDb(field.type, child(i), use, precision);
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

absl::StatusOr<std::string> DuckDbFieldType(const FieldSchema& field, TypeUse use,
                                            TimestampPrecision precision) {
  googlesql::TypeFactory type_factory;
  absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(field, &type_factory);
  if (!type.ok()) {
    return type.status();
  }
  absl::StatusOr<googlesql::TypeParameters> parameters = FieldTypeParameters(field);
  if (!parameters.ok()) {
    return parameters.status();
  }
  std::optional<std::string> duckdb_type = MapToDuckDb(*type, &*parameters, use, precision);
  if (duckdb_type.has_value()) {
    return *std::move(duckdb_type);
  }
  return absl::InvalidArgumentError("Unsupported field type: " +
                                    (*type)->TypeName(googlesql::PRODUCT_EXTERNAL));
}

// Sets the collations of `field` and its fields on `annotations`, which annotate its type; a
// REPEATED field's collation annotates its elements.
void AnnotateCollations(const FieldSchema& field, googlesql::AnnotationMap& annotations) {
  googlesql::AnnotationMap* value = &annotations;
  if (field.mode == FieldMode::kRepeated) {
    value = annotations.AsStructMap()->mutable_field(0);
  }
  // An empty collation compares as none does.
  if (field.collation.has_value() && !field.collation->empty()) {
    value->SetAnnotation(static_cast<int>(googlesql::AnnotationKind::kCollation),
                         googlesql::SimpleValue::String(*field.collation));
  }
  if (googlesql::StructAnnotationMap* fields = value->AsStructMap()) {
    for (int i = 0; i < fields->num_fields() && std::cmp_less(i, field.fields.size()); ++i) {
      AnnotateCollations(field.fields.at(i), *fields->mutable_field(i));
    }
  }
}

}  // namespace

absl::StatusOr<googlesql::AnnotatedType> GoogleSqlAnnotatedType(
    const FieldSchema& field, googlesql::TypeFactory* type_factory) {
  absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(field, type_factory);
  if (!type.ok()) {
    return type.status();
  }
  std::unique_ptr<googlesql::AnnotationMap> annotations = googlesql::AnnotationMap::Create(*type);
  AnnotateCollations(field, *annotations);
  if (annotations->Empty()) {
    return googlesql::AnnotatedType(*type, nullptr);
  }
  absl::StatusOr<const googlesql::AnnotationMap*> owned =
      type_factory->TakeOwnership(std::move(annotations));
  if (!owned.ok()) {
    return owned.status();
  }
  return googlesql::AnnotatedType(*type, *owned);
}

void SetCollations(FieldSchema& field, const googlesql::AnnotationMap* annotations) {
  if (annotations != nullptr && field.mode == FieldMode::kRepeated) {
    const googlesql::StructAnnotationMap* element = annotations->AsStructMap();
    annotations = element == nullptr || element->num_fields() != 1 ? nullptr : element->field(0);
  }
  if (annotations == nullptr) {
    return;
  }
  if (const googlesql::SimpleValue* collation =
          annotations->GetAnnotation(static_cast<int>(googlesql::AnnotationKind::kCollation));
      collation != nullptr && collation->has_string_value() && !collation->string_value().empty()) {
    field.collation = collation->string_value();
  }
  if (const googlesql::StructAnnotationMap* fields = annotations->AsStructMap()) {
    for (int i = 0; i < fields->num_fields() && std::cmp_less(i, field.fields.size()); ++i) {
      SetCollations(field.fields.at(i), fields->field(i));
    }
  }
}

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
    // DuckDB's SQL parser cannot represent a NUL in an identifier. JSON backends recover
    // the original names from the value argument's metadata instead.
    positional = positional || field.name.empty() || field.name.starts_with('$') ||
                 field.name.find('\0') != std::string::npos ||
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

std::optional<std::string> DuckDbType(const googlesql::Type* type, TimestampPrecision precision) {
  return MapToDuckDb(type, nullptr, TypeUse::kQuery, precision);
}

bool HasTimestamp(const googlesql::Type* type, bool range_bounds) {
  if (type->IsTimestamp()) {
    return true;
  }
  if (type->IsArray()) {
    return HasTimestamp(type->AsArray()->element_type(), range_bounds);
  }
  if (type->IsRange()) {
    return range_bounds && HasTimestamp(type->AsRange()->element_type(), range_bounds);
  }
  if (type->IsStruct()) {
    return std::ranges::any_of(type->AsStruct()->fields(), [range_bounds](const auto& field) {
      return HasTimestamp(field.type, range_bounds);
    });
  }
  return false;
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
                                            const googlesql::TypeParameters* parameters,
                                            TimestampPrecision precision) {
  return MapToDuckDb(type, parameters, TypeUse::kDefinition, precision);
}

absl::StatusOr<std::string> DuckDbColumnType(const FieldSchema& field,
                                             TimestampPrecision precision) {
  return DuckDbFieldType(field, TypeUse::kColumn, precision);
}

absl::StatusOr<std::string> DuckDbParameterType(const FieldSchema& field,
                                                TimestampPrecision precision) {
  return DuckDbFieldType(field, TypeUse::kParameter, precision);
}

}  // namespace bigquery_emulator_duckdb
