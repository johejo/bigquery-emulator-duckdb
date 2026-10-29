#include "src/duckdb_types.h"

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "duckdb.h"
#include "src/duckdb_handle.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/timestamp_precision.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {
namespace {

// Maps a DuckDB type to the BigQuery type of a TableFieldSchema.
FieldType ToBigQueryType(duckdb_logical_type type) {
  if (IsRange(type)) {
    return FieldType::kRange;
  }
  if (IsTimestampNanos(type)) {
    return FieldType::kTimestamp;
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
  if (IsTimestampNanos(type)) {
    return std::nullopt;
  }
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

bool IsTimestampNanos(duckdb_logical_type type) {
  if (duckdb_get_type_id(type) != DUCKDB_TYPE_HUGEINT) {
    return false;
  }
  const DuckString alias(duckdb_logical_type_get_alias(type));
  return alias != nullptr && std::string_view(alias.get()) == kTimestampNanosType;
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
