#include "src/vector_value.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/timestamp_base.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"

namespace bigquery_emulator_duckdb {

bool IsNull(const duckdb::Vector& vector, size_t row) {
  return !duckdb::FlatVector::Validity(vector).RowIsValid(row);
}

std::string_view VectorStringView(const duckdb::Vector& vector, size_t row) {
  // Short strings live inside string_t; the view must borrow the vector, not a local copy.
  const auto& value = duckdb::FlatVector::GetData<duckdb::string_t>(vector)[row];
  return {value.GetData(), value.GetSize()};
}

std::string VectorString(const duckdb::Vector& vector, size_t row) {
  return std::string(VectorStringView(vector, row));
}

bool IsListLike(const duckdb::LogicalType& type) {
  return type.id() == duckdb::LogicalTypeId::LIST || type.id() == duckdb::LogicalTypeId::ARRAY;
}

const duckdb::LogicalType& ElementType(const duckdb::LogicalType& type) {
  return type.id() == duckdb::LogicalTypeId::ARRAY ? duckdb::ArrayType::GetChildType(type)
                                                   : duckdb::ListType::GetChildType(type);
}

ListElements::ListElements(const duckdb::Vector& vector, const duckdb::LogicalType& type,
                           size_t row)
    : child_(type.id() == duckdb::LogicalTypeId::ARRAY ? duckdb::ArrayVector::GetChild(vector)
                                                       : duckdb::ListVector::GetChild(vector)),
      type_(ElementType(type)),
      entry_(type.id() == duckdb::LogicalTypeId::ARRAY
                 ? duckdb::list_entry_t(row * duckdb::ArrayType::GetSize(type),
                                        duckdb::ArrayType::GetSize(type))
                 : VectorElement<duckdb::list_entry_t>(vector, row)) {}

bool IsFiniteBound(const duckdb::Vector& bound, const duckdb::LogicalType& type, size_t row) {
  return !IsNull(bound, row) && (type.id() == duckdb::LogicalTypeId::DATE
                                     ? VectorElement<duckdb::date_t>(bound, row).IsFinite()
                                     : VectorElement<duckdb::timestamp_t>(bound, row).IsFinite());
}

duckdb::Value VectorValue(const duckdb::Vector& vector, const duckdb::LogicalType& /*type*/,
                          size_t row) {
  return vector.GetValue(row);
}

std::string ValueString(const duckdb::Value& value) { return value.ToString(); }

}  // namespace bigquery_emulator_duckdb
