#include "src/googlesql_functions_duckdb/any_arguments.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/numeric/int128.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "duckdb.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/numeric_value.h"
#include "googlesql/public/options.pb.h"
#include "nlohmann/json.hpp"
#include "src/bignumeric.h"
#include "src/duckdb_handle.h"
#include "src/duckdb_types.h"
#include "src/googlesql_functions/datetime.h"
#include "src/interval.h"
#include "src/language_options.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::functions_duckdb {
using namespace googlesql_functions;
namespace {

// How GoogleSQL reads JSON values under the analyzer's language options, which round wide numbers
// unless strict number parsing is on.
googlesql::JSONParsingOptions::WideNumberMode JsonNumberMode() {
  return GoogleSqlLanguageOptions().LanguageFeatureEnabled(
             googlesql::FEATURE_JSON_STRICT_NUMBER_PARSING)
             ? googlesql::JSONParsingOptions::WideNumberMode::kExact
             : googlesql::JSONParsingOptions::WideNumberMode::kRound;
}

// JSON goes in and out of the JSON functions as its text.
absl::StatusOr<googlesql::JSONValue> ParseJson(const std::string& text) {
  return googlesql::JSONValue::ParseJSONString(text, {.wide_number_mode = JsonNumberMode()});
}

// The GoogleSQL type that the DuckDB type `type` stands for, the inverse of DuckDbType in
// src/type_mapping.cc.
absl::StatusOr<const googlesql::Type*> GoogleSqlTypeOf(duckdb_logical_type type,
                                                       googlesql::TypeFactory& factory,
                                                       const std::vector<std::string>& names,
                                                       size_t& next_name) {
  const DuckString alias(duckdb_logical_type_get_alias(type));
  if (alias != nullptr && std::string_view(alias.get()) == "JSON") {
    return googlesql::types::JsonType();
  }
  switch (duckdb_get_type_id(type)) {
    case DUCKDB_TYPE_BOOLEAN:
      return googlesql::types::BoolType();
    case DUCKDB_TYPE_BIGINT:
      return googlesql::types::Int64Type();
    case DUCKDB_TYPE_DOUBLE:
      return googlesql::types::DoubleType();
    case DUCKDB_TYPE_VARCHAR:
      return googlesql::types::StringType();
    case DUCKDB_TYPE_BLOB:
      return googlesql::types::BytesType();
    case DUCKDB_TYPE_DATE:
      return googlesql::types::DateType();
    case DUCKDB_TYPE_TIME:
      return googlesql::types::TimeType();
    case DUCKDB_TYPE_TIMESTAMP:
      return googlesql::types::DatetimeType();
    case DUCKDB_TYPE_TIMESTAMP_TZ:
      return googlesql::types::TimestampType();
    case DUCKDB_TYPE_INTERVAL:
      return googlesql::types::IntervalType();
    case DUCKDB_TYPE_BIGNUM:
      return googlesql::types::BigNumericType();
    case DUCKDB_TYPE_DECIMAL:
      if (duckdb_decimal_width(type) == 38 && duckdb_decimal_scale(type) == 9) {
        return googlesql::types::NumericType();
      }
      break;
    case DUCKDB_TYPE_LIST: {
      LogicalType const child(duckdb_list_type_child_type(type));
      const auto element = GoogleSqlTypeOf(child.get(), factory, names, next_name);
      if (!element.ok()) {
        return element.status();
      }
      const auto array = factory.MakeArrayType(*element);
      if (!array.ok()) {
        return array.status();
      }
      return *array;
    }
    case DUCKDB_TYPE_STRUCT: {
      // A STRUCT<> keeps a placeholder field, which contributes no names.
      if (IsEmptyStruct(type)) {
        return googlesql::types::EmptyStructType();
      }
      // A RANGE is a STRUCT whose field names no STRUCT can have, and contributes no names.
      if (IsRange(type)) {
        LogicalType const child(duckdb_struct_type_child_type(type, 0));
        const auto element = GoogleSqlTypeOf(child.get(), factory, names, next_name);
        if (!element.ok()) {
          return element.status();
        }
        const googlesql::Type* range = nullptr;
        if (absl::Status status = factory.MakeRangeType(*element, &range); !status.ok()) {
          return status;
        }
        return range;
      }
      std::vector<googlesql::StructField> fields;
      for (idx_t i = 0; i < duckdb_struct_type_child_count(type); ++i) {
        LogicalType const child(duckdb_struct_type_child_type(type, i));
        const std::string& name = names.at(next_name++);
        const auto field = GoogleSqlTypeOf(child.get(), factory, names, next_name);
        if (!field.ok()) {
          return field.status();
        }
        fields.emplace_back(name, *field);
      }
      const googlesql::StructType* type_struct = nullptr;
      if (absl::Status status = factory.MakeStructType(fields, &type_struct); !status.ok()) {
        return status;
      }
      return type_struct;
    }
    default:
      break;
  }
  return absl::UnimplementedError("Unsupported argument type for a value function");
}

// The value of `type`, the GoogleSQL type of the DuckDB type `duck_type`, in `row` of `vector`.
absl::StatusOr<googlesql::Value> ValueOf(duckdb_vector vector, duckdb_logical_type duck_type,
                                         const googlesql::Type* type, idx_t row) {
  uint64_t* validity = duckdb_vector_get_validity(vector);
  if (validity != nullptr && !duckdb_validity_row_is_valid(validity, row)) {
    return googlesql::Value::Null(type);
  }
  switch (type->kind()) {
    case googlesql::TYPE_BOOL:
      return googlesql::Value::Bool(VectorElement<bool>(vector, row));
    case googlesql::TYPE_INT64:
      return googlesql::Value::Int64(VectorElement<int64_t>(vector, row));
    case googlesql::TYPE_DOUBLE:
      return googlesql::Value::Double(VectorElement<double>(vector, row));
    case googlesql::TYPE_STRING:
      return googlesql::Value::String(VectorString(vector, row));
    case googlesql::TYPE_BYTES:
      return googlesql::Value::Bytes(VectorString(vector, row));
    case googlesql::TYPE_DATE:
      return googlesql::Value::Date(VectorElement<int32_t>(vector, row));
    case googlesql::TYPE_TIME:
      return googlesql::Value::Time(TimeFromMicros(VectorElement<int64_t>(vector, row)));
    case googlesql::TYPE_DATETIME: {
      const auto datetime = DatetimeFromMicros(VectorElement<int64_t>(vector, row));
      if (!datetime.ok()) {
        return datetime.status();
      }
      return googlesql::Value::Datetime(*datetime);
    }
    case googlesql::TYPE_TIMESTAMP:
      return googlesql::Value::Timestamp(absl::FromUnixMicros(VectorElement<int64_t>(vector, row)));
    case googlesql::TYPE_INTERVAL: {
      const auto interval = IntervalFromDuckDb(VectorElement<duckdb_interval>(vector, row));
      if (!interval.ok()) {
        return interval.status();
      }
      return googlesql::Value::Interval(*interval);
    }
    case googlesql::TYPE_NUMERIC: {
      // A DECIMAL(38, 9), stored as a 128-bit integer of units of 10^-9.
      const auto value = VectorElement<duckdb_hugeint>(vector, row);
      const absl::int128 units = absl::MakeInt128(value.upper, value.lower);
      std::string digits = absl::StrCat(units < 0 ? -units : units);
      const size_t scale = duckdb_decimal_scale(duck_type);
      digits.insert(0, scale + 1 > digits.size() ? scale + 1 - digits.size() : 0, '0');
      digits.insert(digits.size() - scale, ".");
      if (units < 0) {
        digits.insert(0, "-");
      }
      const auto numeric = googlesql::NumericValue::FromString(digits);
      return numeric.ok() ? absl::StatusOr<googlesql::Value>(googlesql::Value::Numeric(*numeric))
                          : numeric.status();
    }
    case googlesql::TYPE_BIGNUMERIC: {
      const auto bignumeric = BigNumericFromBignum(VectorString(vector, row));
      return bignumeric.ok()
                 ? absl::StatusOr<googlesql::Value>(googlesql::Value::BigNumeric(*bignumeric))
                 : bignumeric.status();
    }
    case googlesql::TYPE_JSON: {
      auto json = ParseJson(VectorString(vector, row));
      if (!json.ok()) {
        return json.status();
      }
      return googlesql::Value::Json(*std::move(json));
    }
    case googlesql::TYPE_ARRAY: {
      LogicalType const child_type(duckdb_list_type_child_type(duck_type));
      const auto entry = VectorElement<duckdb_list_entry>(vector, row);
      duckdb_vector child = duckdb_list_vector_get_child(vector);
      std::vector<googlesql::Value> elements;
      for (idx_t i = 0; i < entry.length; ++i) {
        auto element =
            ValueOf(child, child_type.get(), type->AsArray()->element_type(), entry.offset + i);
        if (!element.ok()) {
          return element.status();
        }
        elements.push_back(*std::move(element));
      }
      return googlesql::Value::MakeArray(type->AsArray(), std::move(elements));
    }
    case googlesql::TYPE_RANGE: {
      // Unbounded ends are infinite in DuckDB and NULL in GoogleSQL.
      const googlesql::Type* element_type = type->AsRange()->element_type();
      LogicalType const child_type(duckdb_struct_type_child_type(duck_type, 0));
      std::vector<googlesql::Value> bounds;
      for (idx_t i = 0; i < 2; ++i) {
        duckdb_vector child = duckdb_struct_vector_get_child(vector, i);
        const bool finite = element_type->IsDate()
                                ? duckdb_is_finite_date({VectorElement<int32_t>(child, row)})
                                : duckdb_is_finite_timestamp({VectorElement<int64_t>(child, row)});
        auto bound = finite ? ValueOf(child, child_type.get(), element_type, row)
                            : googlesql::Value::Null(element_type);
        if (!bound.ok()) {
          return bound.status();
        }
        bounds.push_back(*std::move(bound));
      }
      return googlesql::Value::MakeRange(bounds.at(0), bounds.at(1));
    }
    case googlesql::TYPE_STRUCT: {
      std::vector<googlesql::Value> fields;
      for (int i = 0; i < type->AsStruct()->num_fields(); ++i) {
        LogicalType const child_type(duckdb_struct_type_child_type(duck_type, i));
        auto field = ValueOf(duckdb_struct_vector_get_child(vector, i), child_type.get(),
                             type->AsStruct()->field(i).type, row);
        if (!field.ok()) {
          return field.status();
        }
        fields.push_back(*std::move(field));
      }
      return googlesql::Value::MakeStruct(type->AsStruct(), std::move(fields));
    }
    default:
      return absl::UnimplementedError("Unsupported argument type for a value function");
  }
}

}  // namespace

AnyArguments::AnyArguments(duckdb_data_chunk input, const std::vector<idx_t>& paired) {
  if (duckdb_data_chunk_get_size(input) == 0) {
    return;
  }
  for (idx_t column = 0; column < duckdb_data_chunk_get_column_count(input); ++column) {
    duckdb_vector vector = duckdb_data_chunk_get_vector(input, column);
    LogicalType type(duckdb_vector_get_column_type(vector));
    std::vector<std::string> names;
    if (paired.empty() || std::ranges::find(paired, column) != paired.end()) {
      names = nlohmann::json::parse(VectorString(duckdb_struct_vector_get_child(vector, 1), 0))
                  .get<std::vector<std::string>>();
      vector = duckdb_struct_vector_get_child(vector, 0);
      duck_types_.emplace_back(duckdb_struct_type_child_type(type.get(), 0));
    } else {
      duck_types_.push_back(std::move(type));
    }
    size_t next_name = 0;
    auto const googlesql_type =
        GoogleSqlTypeOf(duck_types_.back().get(), factory_, names, next_name);
    if (!googlesql_type.ok()) {
      status_ = googlesql_type.status();
    }
    types_.push_back(googlesql_type.value_or(nullptr));
    vectors_.push_back(vector);
  }
}

absl::StatusOr<googlesql::Value> AnyArguments::Get(idx_t column, idx_t row) const {
  if (!status_.ok()) {
    return status_;
  }
  return ValueOf(vectors_.at(column), duck_types_.at(column).get(), types_.at(column), row);
}

const googlesql::Type* AnyArguments::Type(idx_t column) const {
  return column < types_.size() ? types_.at(column) : nullptr;
}

}  // namespace bigquery_emulator_duckdb::functions_duckdb
