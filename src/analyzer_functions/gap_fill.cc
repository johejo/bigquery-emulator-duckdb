#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "googlesql/public/function.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "src/analyzer_functions/declarations.h"
#include "src/language_options.h"

namespace bigquery_emulator_duckdb::analyzer_functions {
namespace {

absl::Status Unsupported(const std::string& what) {
  return absl::InvalidArgumentError("The emulator does not support GAP_FILL " + what);
}

absl::Status Invalid(const std::string& message) {
  return absl::InvalidArgumentError("Invalid table-valued function GAP_FILL\n" + message);
}

// BigQuery's GAP_FILL is not in GoogleSQL's built-in catalog. Its output has the time series
// column, then the partitioning columns and the value columns in the order the arguments list
// them; without value_columns, every other input column is a value column.
absl::StatusOr<std::shared_ptr<googlesql::TVFSignature>> GapFillSchema(
    googlesql::Catalog* /*catalog*/, googlesql::TypeFactory* /*type_factory*/,
    const googlesql::FunctionSignature& /*signature*/,
    const std::vector<googlesql::TVFInputArgumentType>& arguments,
    const googlesql::AnalyzerOptions& /*options*/) {
  const auto& input = arguments.at(0).relation();
  if (input.is_value_table()) {
    return Unsupported("value table");
  }
  // The literal of each scalar argument, absent for an omitted one. GoogleSQL resolves an INTERVAL
  // literal as an expression, which the translation checks.
  constexpr size_t kWidth = 1;
  constexpr size_t kOrigin = 3;
  std::vector<std::optional<googlesql::Value>> literals(6);
  const googlesql::Type* origin = nullptr;
  for (size_t i = 1; i < arguments.size() && i <= literals.size(); ++i) {
    const auto scalar = arguments.at(i).GetScalarArgType();
    if (!scalar.ok()) {
      return scalar.status();
    }
    if (i - 1 == kOrigin) {
      // BigQuery numbers arguments in the documented order, which puts origin sixth. An omitted
      // origin is an untyped NULL, as an explicit NULL is.
      if (scalar->literal_value() == nullptr) {
        return Invalid("Argument 6 must a literal");
      }
      if (!scalar->literal_value()->is_null()) {
        origin = scalar->type();
      }
    } else if (i - 1 == kWidth) {
      // BigQuery numbers arguments in the documented order, which puts bucket_width third.
      if (scalar->literal_value() != nullptr && scalar->literal_value()->is_null()) {
        return Invalid("Argument 3 must be non-NULL");
      }
    } else {
      if (scalar->literal_value() == nullptr) {
        return Unsupported("non-literal argument");
      }
      literals.at(i - 1) = *scalar->literal_value();
    }
  }
  const auto& ts_name = literals.at(0);
  const auto& values = literals.at(2);
  const auto& partitions = literals.at(4);
  const auto& ignore_nulls = literals.at(5);
  // The catalog's default keeps an omitted ignore_null_values apart from an explicit NULL. A NULL
  // value_columns or partitioning_columns is its default.
  if (ignore_nulls && ignore_nulls->is_null()) {
    return Invalid("Argument 7 must be non-NULL");
  }
  if (!ts_name || ts_name->is_null()) {
    return Unsupported("NULL argument");
  }
  const auto find = [&](const std::string& name) -> const googlesql::TVFRelation::Column* {
    const googlesql::TVFRelation::Column* found = nullptr;
    for (const auto& column : input.columns()) {
      if (absl::EqualsIgnoreCase(column.name, name)) {
        if (found != nullptr) {
          return nullptr;
        }
        found = &column;
      }
    }
    return found;
  };
  const auto* ts = find(ts_name->string_value());
  if (ts == nullptr) {
    return absl::InvalidArgumentError("GAP_FILL requires an unambiguous time series column");
  }
  if (!ts->type->IsDate() && !ts->type->IsDatetime() && !ts->type->IsTimestamp()) {
    return absl::InvalidArgumentError(
        "GAP_FILL requires a time series column of type DATE, DATETIME or TIMESTAMP");
  }
  if (origin != nullptr && !origin->Equals(ts->type)) {
    return absl::InvalidArgumentError(
        "GAP_FILL requires origin of the same type as the time series column");
  }
  std::vector<googlesql::TVFRelation::Column> columns = {*ts};
  const auto listed = [&](const std::string& name) {
    return std::ranges::any_of(columns, [&](const googlesql::TVFRelation::Column& existing) {
      return absl::EqualsIgnoreCase(existing.name, name);
    });
  };
  const auto add = [&](const googlesql::TVFRelation::Column* column) -> absl::Status {
    if (listed(column->name)) {
      return absl::InvalidArgumentError("GAP_FILL column " + column->name +
                                        " is listed more than once");
    }
    columns.push_back(*column);
    return absl::OkStatus();
  };
  std::vector<googlesql::Value> partition_names;
  if (partitions && !partitions->is_null()) {
    const auto elements = partitions->elements_view();
    partition_names.assign(elements->begin(), elements->end());
  }
  for (const auto& name : partition_names) {
    const auto* column = name.is_null() ? nullptr : find(name.string_value());
    if (column == nullptr) {
      return absl::InvalidArgumentError("GAP_FILL requires unambiguous partitioning columns");
    }
    if (column->type->IsInterval() || column->type->IsStruct() || column->type->IsArray() ||
        !column->type->SupportsPartitioning(GoogleSqlLanguageOptions())) {
      return Unsupported("partitioning column type " +
                         column->type->TypeName(googlesql::PRODUCT_EXTERNAL));
    }
    if (auto status = add(column); !status.ok()) {
      return status;
    }
  }
  const size_t partition_count = columns.size();
  if (!values || values->is_null()) {
    std::vector<googlesql::TVFRelation::Column> others;
    std::ranges::copy_if(input.columns(), std::back_inserter(others),
                         [&](const auto& column) { return !listed(column.name); });
    columns.insert(columns.end(), others.begin(), others.end());
  } else {
    const auto pairs = values->elements_view();
    for (const auto& pair : *pairs) {
      if (pair.is_null() || pair.field(0).is_null() || pair.field(1).is_null()) {
        return Unsupported("NULL value column");
      }
      const auto* column = find(pair.field(0).string_value());
      if (column == nullptr) {
        return absl::InvalidArgumentError("GAP_FILL requires unambiguous value columns");
      }
      // Method names are case-insensitive.
      const std::string& method = pair.field(1).string_value();
      const bool linear = absl::EqualsIgnoreCase(method, "linear");
      if (!linear && !absl::EqualsIgnoreCase(method, "null") &&
          !absl::EqualsIgnoreCase(method, "locf")) {
        return Unsupported("gap-filling method " + method);
      }
      const std::string type = column->type->TypeName(googlesql::PRODUCT_EXTERNAL);
      if (linear && !column->type->IsNumerical()) {
        return Invalid("Column " + column->name + " (" + type +
                       ") is not allowed for linear interpolation. Only columns with numeric "
                       "types are allowed");
      }
      // Linear interpolation of NUMERIC and BIGNUMERIC needs their rounded arithmetic.
      if (linear && !column->type->IsInt64() && !column->type->IsDouble()) {
        return Unsupported("linear gap filling of " + type);
      }
      if (auto status = add(column); !status.ok()) {
        return status;
      }
    }
  }
  for (size_t i = partition_count; i < columns.size(); ++i) {
    // Rows of a time point are condensed when their values are equal, which needs equality.
    if (!columns.at(i).type->SupportsEquality(GoogleSqlLanguageOptions())) {
      return Unsupported("value column type " +
                         columns.at(i).type->TypeName(googlesql::PRODUCT_EXTERNAL));
    }
  }
  return googlesql::TVFSignature::Create(arguments, googlesql::TVFRelation(std::move(columns)));
}

googlesql::FunctionArgumentType Named(const googlesql::Type* type, const char* name,
                                      googlesql::FunctionEnums::NamedArgumentKind kind,
                                      std::optional<googlesql::Value> value = std::nullopt) {
  googlesql::FunctionArgumentTypeOptions options(kind == googlesql::kNamedOnly
                                                     ? googlesql::FunctionArgumentType::OPTIONAL
                                                     : googlesql::FunctionArgumentType::REQUIRED);
  options.set_argument_name(name, kind);
  if (value) {
    options.set_default(*value);
  }
  return {type, options};
}

}  // namespace

void AddGapFill(googlesql::SimpleCatalog& catalog) {
  const googlesql::StructType* pair = nullptr;
  if (!catalog.type_factory()
           ->MakeStructType(
               {{"", googlesql::types::StringType()}, {"", googlesql::types::StringType()}}, &pair)
           .ok()) {
    throw std::runtime_error("GAP_FILL argument types");
  }
  const absl::StatusOr<const googlesql::ArrayType*> array =
      catalog.type_factory()->MakeArrayType(pair);
  if (!array.ok()) {
    throw std::runtime_error("GAP_FILL argument types");
  }
  const auto* pairs = *array;
  const auto* string_array = googlesql::types::StringArrayType();
  googlesql::FunctionArgumentTypeOptions origin(googlesql::FunctionArgumentType::OPTIONAL);
  origin.set_argument_name("origin", googlesql::kNamedOnly);
  catalog.AddOwnedTableValuedFunction(std::make_unique<googlesql::TableValuedFunction>(
      std::vector<std::string>{"gap_fill"}, kBigQueryGroup,
      std::vector<googlesql::FunctionSignature>{
          googlesql::FunctionSignature(
              googlesql::ARG_KIND_RELATION,
              {
                  googlesql::FunctionArgumentType::AnyRelation(),
                  Named(googlesql::types::StringType(), "ts_column", googlesql::kPositionalOrNamed),
                  Named(googlesql::types::IntervalType(), "bucket_width",
                        googlesql::kPositionalOrNamed),
                  // Arguments with defaults must follow the others; named-only arguments
                  // can be given in any order.
                  Named(pairs, "value_columns", googlesql::kNamedOnly),
                  googlesql::FunctionArgumentType(googlesql::ARG_KIND_EXPR_ARBITRARY, origin),
                  Named(string_array, "partitioning_columns", googlesql::kNamedOnly,
                        googlesql::Value::EmptyArray(string_array)),
                  Named(googlesql::types::BoolType(), "ignore_null_values", googlesql::kNamedOnly,
                        googlesql::Value::Bool(true)),
              },
              /*context_id=*/int64_t{0}),
      },
      googlesql::TableValuedFunctionOptions().set_compute_result_type_callback(GapFillSchema)));
}

}  // namespace bigquery_emulator_duckdb::analyzer_functions
