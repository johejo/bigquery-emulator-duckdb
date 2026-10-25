#include "src/column_default.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/reference_impl/type_parameter_constraints.h"
#include "src/api_error.h"
#include "src/field_schema.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {
namespace {

bool HasDefaultTypeConstraints(const FieldSchema& field) {
  return field.max_length.has_value() || field.precision.has_value() || field.scale.has_value() ||
         std::ranges::any_of(field.fields, HasDefaultTypeConstraints);
}

}  // namespace

googlesql::Value RoundedColumnDefault(const FieldSchema& field, const googlesql::Value& value) {
  if (value.is_null() || !RoundsToEven(field)) {
    return value;
  }
  if (value.type()->IsArray()) {
    std::vector<googlesql::Value> elements;
    elements.reserve(value.num_elements());
    for (int i = 0; i < value.num_elements(); ++i) {
      elements.push_back(RoundedColumnDefault(field, value.element(i)));
    }
    return googlesql::Value::MakeArray(value.type()->AsArray(), elements).value();
  }
  if (value.type()->IsStruct()) {
    std::vector<googlesql::Value> fields;
    fields.reserve(value.num_fields());
    for (int i = 0; i < value.num_fields(); ++i) {
      fields.push_back(RoundedColumnDefault(field.fields.at(i), value.field(i)));
    }
    return googlesql::Value::MakeStruct(value.type()->AsStruct(), fields).value();
  }
  if (value.type()->IsNumericType()) {
    const auto rounded = value.numeric_value().Round(field.scale.value_or(0), true);
    if (!rounded.ok()) {
      throw ApiError::Invalid(std::string(rounded.status().message()));
    }
    return googlesql::Value::Numeric(*rounded);
  }
  const auto rounded = value.bignumeric_value().Round(field.scale.value_or(0), true);
  if (!rounded.ok()) {
    throw ApiError::Invalid(std::string(rounded.status().message()));
  }
  return googlesql::Value::BigNumeric(*rounded);
}

void ValidateColumnDefault(const FieldSchema& field,
                           const std::optional<googlesql::Value>& literal_value) {
  const auto parameters = FieldTypeParameters(field, /*include_lengths=*/true);
  if (!parameters.ok()) {
    throw ApiError::Invalid(std::string(parameters.status().message()));
  }
  if (HasDefaultTypeConstraints(field) && !literal_value.has_value()) {
    throw ApiError::Unsupported("non-literal defaults on parameterized columns");
  }
  if (literal_value.has_value()) {
    auto literal = RoundedColumnDefault(field, *literal_value);
    if (literal.is_null() && field.mode == FieldMode::kRequired) {
      throw ApiError::Invalid("NULL default for required column " + field.name);
    }
    const auto status =
        googlesql::ApplyConstraints(*parameters, googlesql::PRODUCT_EXTERNAL, literal);
    if (!status.ok()) {
      throw ApiError::Invalid(
          std::format("Invalid value: Field {}: {}", field.name, status.message()));
    }
  }
}

}  // namespace bigquery_emulator_duckdb
