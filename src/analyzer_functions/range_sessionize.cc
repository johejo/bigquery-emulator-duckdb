#include <cstddef>
#include <cstdint>
#include <memory>
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

// BigQuery's RANGE_SESSIONIZE is not in GoogleSQL's built-in catalog. Its output
// retains the input schema and appends a RANGE of the selected column's type.
absl::StatusOr<std::shared_ptr<googlesql::TVFSignature>> SessionizeSchema(
    googlesql::Catalog* /*catalog*/, googlesql::TypeFactory* /*type_factory*/,
    const googlesql::FunctionSignature& /*signature*/,
    const std::vector<googlesql::TVFInputArgumentType>& arguments,
    const googlesql::AnalyzerOptions& /*options*/) {
  const auto& input = arguments.at(0).relation();
  if (input.is_value_table()) {
    return absl::InvalidArgumentError("The emulator does not support RANGE_SESSIONIZE value table");
  }
  std::vector<googlesql::Value> literals;
  for (size_t i = 1; i < arguments.size(); ++i) {
    const auto scalar = arguments.at(i).GetScalarArgType();
    if (!scalar.ok() || scalar->literal_value() == nullptr || scalar->literal_value()->is_null()) {
      return absl::InvalidArgumentError(
          "The emulator does not support RANGE_SESSIONIZE non-literal or NULL argument");
    }
    literals.push_back(*scalar->literal_value());
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
  const auto* range = find(literals.at(0).string_value());
  if (range == nullptr || !range->type->IsRange()) {
    return absl::InvalidArgumentError("RANGE_SESSIONIZE requires an unambiguous RANGE column");
  }
  const auto partition_names = literals.at(1).elements_view();
  for (const auto& name : *partition_names) {
    const auto* column = name.is_null() ? nullptr : find(name.string_value());
    if (column == nullptr) {
      return absl::InvalidArgumentError(
          "RANGE_SESSIONIZE requires unambiguous partitioning columns");
    }
    if (!column->type->SupportsGrouping(GoogleSqlLanguageOptions())) {
      return absl::InvalidArgumentError(
          "The emulator does not support RANGE_SESSIONIZE partitioning column type");
    }
  }
  if (literals.size() == 3 && literals.at(2).EnumDisplayName() != "MEETS" &&
      literals.at(2).EnumDisplayName() != "OVERLAPS") {
    return absl::InvalidArgumentError("RANGE_SESSIONIZE option must be MEETS or OVERLAPS");
  }
  auto columns = input.columns();
  bool replaced = false;
  for (auto& column : columns) {
    if (absl::EqualsIgnoreCase(column.name, "session_range")) {
      column = googlesql::TVFRelation::Column("session_range", range->type);
      replaced = true;
    }
  }
  if (!replaced) {
    columns.emplace_back("session_range", range->type);
  }
  return googlesql::TVFSignature::Create(arguments, googlesql::TVFRelation(std::move(columns)));
}

}  // namespace

void AddRangeSessionize(googlesql::SimpleCatalog& catalog) {
  catalog.AddOwnedTableValuedFunction(std::make_unique<googlesql::TableValuedFunction>(
      std::vector<std::string>{"range_sessionize"}, kBigQueryGroup,
      std::vector<googlesql::FunctionSignature>{
          googlesql::FunctionSignature(
              googlesql::ARG_KIND_RELATION,
              {
                  googlesql::FunctionArgumentType::AnyRelation(),
                  googlesql::FunctionArgumentType(googlesql::types::StringType()),
                  googlesql::FunctionArgumentType(googlesql::types::StringArrayType()),
                  googlesql::FunctionArgumentType(googlesql::types::RangeSessionizeModeEnumType(),
                                                  googlesql::FunctionArgumentType::OPTIONAL),
              },
              /*context_id=*/int64_t{0}),
      },
      googlesql::TableValuedFunctionOptions().set_compute_result_type_callback(SessionizeSchema)));
}

}  // namespace bigquery_emulator_duckdb::analyzer_functions
