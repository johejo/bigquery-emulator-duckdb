#include "src/duckdb_types.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "duckdb.h"
#include "duckdb/common/types.hpp"
#include "src/duckdb_capi.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/timestamp_precision.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {
namespace {

using duckdb::LogicalType;
using duckdb::LogicalTypeId;

bool IsListLike(const LogicalType& type) {
  return type.id() == LogicalTypeId::LIST || type.id() == LogicalTypeId::ARRAY;
}

const LogicalType& ElementType(const LogicalType& type) {
  return type.id() == LogicalTypeId::ARRAY ? duckdb::ArrayType::GetChildType(type)
                                           : duckdb::ListType::GetChildType(type);
}

// Maps a DuckDB type to the BigQuery type of a TableFieldSchema.
FieldType ToBigQueryType(const LogicalType& type) {
  if (IsRange(type)) {
    return FieldType::kRange;
  }
  if (IsTimestampNanos(type)) {
    return FieldType::kTimestamp;
  }
  switch (type.id()) {
    case LogicalTypeId::BOOLEAN:
      return FieldType::kBoolean;
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::UINTEGER:
      return FieldType::kInteger;
    case LogicalTypeId::UBIGINT:
    case LogicalTypeId::HUGEINT:
    case LogicalTypeId::UHUGEINT:
    case LogicalTypeId::BIGNUM:
      return FieldType::kBigNumeric;
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
      return FieldType::kFloat;
    case LogicalTypeId::DECIMAL:
      return duckdb::DecimalType::GetWidth(type) - duckdb::DecimalType::GetScale(type) <= 29 &&
                     duckdb::DecimalType::GetScale(type) <= 9
                 ? FieldType::kNumeric
                 : FieldType::kBigNumeric;
    case LogicalTypeId::VARCHAR:
    case LogicalTypeId::UUID:
      return FieldType::kString;
    case LogicalTypeId::BLOB:
      return FieldType::kBytes;
    case LogicalTypeId::DATE:
      return FieldType::kDate;
    case LogicalTypeId::TIME:
    case LogicalTypeId::TIME_TZ:
      return FieldType::kTime;
    // BigQuery TIMESTAMP is an absolute instant; DATETIME is a civil time.
    case LogicalTypeId::TIMESTAMP_TZ:
      return FieldType::kTimestamp;
    case LogicalTypeId::TIMESTAMP:
    case LogicalTypeId::TIMESTAMP_SEC:
    case LogicalTypeId::TIMESTAMP_MS:
    case LogicalTypeId::TIMESTAMP_NS:
      return FieldType::kDatetime;
    case LogicalTypeId::INTERVAL:
      return FieldType::kInterval;
    case LogicalTypeId::STRUCT:
      return FieldType::kRecord;
    case LogicalTypeId::LIST:
    case LogicalTypeId::ARRAY:
      return ToBigQueryType(ElementType(type));
    default:
      return FieldType::kString;
  }
}

std::optional<std::string> ReadNumericStorage(const std::string& sql, const LogicalType& type,
                                              int depth) {
  if (IsTimestampNanos(type)) {
    return std::nullopt;
  }
  switch (type.id()) {
    case LogicalTypeId::UBIGINT:
    case LogicalTypeId::HUGEINT:
    case LogicalTypeId::UHUGEINT:
      return "CAST(bq_bignumeric_from_string(CAST(" + sql + " AS VARCHAR), false) AS BIGNUM)";
    case LogicalTypeId::DECIMAL:
      if (ToBigQueryType(type) == FieldType::kBigNumeric) {
        return "CAST(bq_bignumeric_from_string(CAST(" + sql + " AS VARCHAR), false) AS BIGNUM)";
      }
      return std::nullopt;
    case LogicalTypeId::LIST:
    case LogicalTypeId::ARRAY: {
      const std::string element = "_bq_numeric_" + std::to_string(depth);
      const auto converted = ReadNumericStorage(element, ElementType(type), depth + 1);
      if (!converted) {
        return std::nullopt;
      }
      return "list_transform(" + sql + ", lambda " + element + ": " + *converted + ")";
    }
    case LogicalTypeId::STRUCT: {
      std::vector<std::string> fields;
      bool changed = false;
      for (const auto& [name, child] : duckdb::StructType::GetChildTypes(type)) {
        const std::string field = QuoteIdentifier(name.GetIdentifierName());
        const std::string access = std::format("({}).{}", sql, field);
        const auto converted = ReadNumericStorage(access, child, depth + 1);
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

bool IsTimestampNanos(const duckdb::LogicalType& type) {
  return type.id() == LogicalTypeId::HUGEINT && type.GetAlias() == kTimestampNanosType;
}

bool IsRange(const duckdb::LogicalType& type) {
  return type.id() == LogicalTypeId::STRUCT && duckdb::StructType::GetChildCount(type) == 2 &&
         duckdb::StructType::GetChildName(type, 0).GetIdentifierName() == kRangeStart &&
         duckdb::StructType::GetChildName(type, 1).GetIdentifierName() == kRangeEnd;
}

bool IsEmptyStruct(const duckdb::LogicalType& type) {
  return type.id() == LogicalTypeId::STRUCT && duckdb::StructType::GetChildCount(type) == 1 &&
         duckdb::StructType::GetChildName(type, 0).GetIdentifierName() == kEmptyStruct;
}

// C callbacks still borrow their argument types through the V1 API.
bool IsRange(duckdb_logical_type type) { return IsRange(BorrowCppLogicalType(type)); }
bool IsEmptyStruct(duckdb_logical_type type) { return IsEmptyStruct(BorrowCppLogicalType(type)); }

FieldSchema ResultFieldSchema(const std::string& name, const duckdb::LogicalType& type) {
  FieldSchema field;
  field.name = name;
  field.type = ToBigQueryType(type);
  field.mode = IsListLike(type) ? FieldMode::kRepeated : FieldMode::kNullable;
  const auto& scalar = IsListLike(type) ? ElementType(type) : type;
  if (IsRange(scalar)) {
    field.range_element_type = ToBigQueryType(duckdb::StructType::GetChildType(scalar, 0));
  } else if (scalar.id() == LogicalTypeId::STRUCT && !IsEmptyStruct(scalar)) {
    for (const auto& [child_name, child_type] : duckdb::StructType::GetChildTypes(scalar)) {
      field.fields.push_back(ResultFieldSchema(child_name.GetIdentifierName(), child_type));
    }
  }
  return field;
}

std::string UnsupportedDuckDbType(const duckdb::LogicalType& type) {
  switch (type.id()) {
    case LogicalTypeId::BIGNUM:
      return type.GetAlias().starts_with("bq_bignumeric_") ? "" : "BIGNUM";
    case LogicalTypeId::LIST:
    case LogicalTypeId::ARRAY:
      return UnsupportedDuckDbType(ElementType(type));
    case LogicalTypeId::STRUCT:
      for (const auto& child : duckdb::StructType::GetChildTypes(type)) {
        if (std::string unsupported = UnsupportedDuckDbType(child.second); !unsupported.empty()) {
          return unsupported;
        }
      }
      return "";
    default:
      return "";
  }
}

bool HasOtherTimestampPrecision(const duckdb::LogicalType& type, TimestampPrecision precision) {
  if (IsTimestampNanos(type)) {
    return precision != TimestampPrecision::kNanoseconds;
  }
  switch (type.id()) {
    case LogicalTypeId::TIMESTAMP_TZ:
      return precision != TimestampPrecision::kMicroseconds;
    case LogicalTypeId::LIST:
    case LogicalTypeId::ARRAY:
      return HasOtherTimestampPrecision(ElementType(type), precision);
    case LogicalTypeId::STRUCT:
      if (IsRange(type)) {
        return false;
      }
      return std::ranges::any_of(duckdb::StructType::GetChildTypes(type),
                                 [precision](const auto& child) {
                                   return HasOtherTimestampPrecision(child.second, precision);
                                 });
    default:
      return false;
  }
}

std::optional<std::string> ReadNumericStorage(const std::string& sql,
                                              const duckdb::LogicalType& type) {
  return ReadNumericStorage(sql, type, 0);
}

}  // namespace bigquery_emulator_duckdb
