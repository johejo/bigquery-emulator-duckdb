#include "src/googlesql_collation_duckdb/collation.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "src/duckdb_functions/apply.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/googlesql_collation/collation.h"

namespace bigquery_emulator_duckdb::collation_duckdb {
using namespace duckdb_functions;
namespace {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): COLLATE(value, specification).
absl::StatusOr<std::string> Collate(std::string_view value, std::string_view specification,
                                    bool bigquery) {
  GOOGLESQL_RETURN_IF_ERROR(googlesql_collation::CheckSpecification(specification, bigquery));
  return std::string(value);
}

// The DISTINCT values under the specification of the list the translator collects with list(),
// with its direction (0 to keep them in input order, 1 to sort them ascending by their keys and
// -1 descending) and whether NULL sorts first. Equal values, and NULL values, occur once, as the
// first of them in input order, as BigQuery keeps them.
void CollationDistinct(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const bool bigquery = FunctionContext<bool>(info);
  EachRow(
      info, input, output,
      [bigquery](const Arguments& args) -> absl::StatusOr<std::vector<std::optional<std::string>>> {
        duckdb_vector vector = args.Vector(0);
        const auto entry = VectorElement<duckdb_list_entry>(vector, args.Row());
        duckdb_vector child = duckdb_list_vector_get_child(vector);
        uint64_t* validity = duckdb_vector_get_validity(child);
        const std::string_view specification = args.StringView(1);
        // Each distinct value with its key, and NULL as nullopt.
        std::vector<std::optional<std::pair<std::string, std::string>>> distinct;
        std::unordered_set<std::string> keys;
        bool null = false;
        for (idx_t i = 0; i < entry.length; ++i) {
          if (validity != nullptr && !duckdb_validity_row_is_valid(validity, entry.offset + i)) {
            if (!std::exchange(null, true)) {
              distinct.emplace_back();
            }
            continue;
          }
          std::string value = VectorString(child, entry.offset + i);
          GOOGLESQL_ASSIGN_OR_RETURN(std::string key,
                                     googlesql_collation::SortKey(value, specification, bigquery));
          if (keys.insert(key).second) {
            distinct.emplace_back(std::in_place, std::move(key), std::move(value));
          }
        }
        const int64_t direction = args.Int(2);
        const bool nulls_first = args.Bool(3);
        if (direction != 0) {
          // Keys are distinct, so the order of equal elements does not matter.
          std::ranges::sort(distinct, [&](const auto& left, const auto& right) {
            if (!left || !right) {
              return nulls_first ? !left && right : left && !right;
            }
            return direction > 0 ? left->first < right->first : right->first < left->first;
          });
        }
        std::vector<std::optional<std::string>> values(distinct.size());
        std::ranges::transform(distinct, values.begin(), [](auto& element) {
          return element ? std::optional(std::move(element->second)) : std::nullopt;
        });
        return values;
      });
}

}  // namespace

void RegisterCollationFunctions(duckdb_connection connection, bool bigquery) {
  RegisterWithContext(connection, "bq_collate", {kVarchar, kVarchar}, kVarchar,
                      ApplyWithContext<Collate>, bigquery);
  RegisterWithContext(connection, "bq_collation_key", {kVarchar, kVarchar}, kBlob,
                      ApplyWithContext<googlesql_collation::SortKey>, bigquery);
  RegisterWithContext(connection, "bq_collation_like", {kVarchar, kVarchar, kVarchar}, kBoolean,
                      ApplyWithContext<googlesql_collation::Like>, bigquery);
  RegisterWithContext(connection, "bq_collation_starts_with", {kVarchar, kVarchar, kVarchar},
                      kBoolean, ApplyWithContext<googlesql_collation::StartsWith>, bigquery);
  RegisterWithContext(connection, "bq_collation_ends_with", {kVarchar, kVarchar, kVarchar},
                      kBoolean, ApplyWithContext<googlesql_collation::EndsWith>, bigquery);
  RegisterWithContext(connection, "bq_collation_instr",
                      {kVarchar, kVarchar, kVarchar, kBigint, kBigint}, kBigint,
                      ApplyWithContext<googlesql_collation::Instr>, bigquery);
  RegisterWithContext(connection, "bq_collation_replace", {kVarchar, kVarchar, kVarchar, kVarchar},
                      kVarchar, ApplyWithContext<googlesql_collation::Replace>, bigquery);
  const LogicalType varchar(duckdb_create_logical_type(kVarchar));
  const LogicalType list(duckdb_create_list_type(varchar.get()));
  RegisterWithContext(connection, "bq_collation_split",
                      {varchar.get(), varchar.get(), varchar.get()}, list.get(),
                      ApplyWithContext<googlesql_collation::Split>, bigquery);
  const LogicalType bigint(duckdb_create_logical_type(kBigint));
  const LogicalType boolean(duckdb_create_logical_type(kBoolean));
  RegisterWithContext(connection, "bq_collation_distinct",
                      {list.get(), varchar.get(), bigint.get(), boolean.get()}, list.get(),
                      CollationDistinct, bigquery);
}

}  // namespace bigquery_emulator_duckdb::collation_duckdb
