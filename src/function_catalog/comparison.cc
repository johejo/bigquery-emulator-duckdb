#include "src/function_catalog/comparison.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <vector>

#include "googlesql/public/type.h"
#include "src/duckdb_sql.h"
#include "src/function_catalog/rule_builders.h"

namespace bigquery_emulator_duckdb::function_catalog {

// Whether `left` = `right`, two values of `type`. BigQuery compares STRUCTs field by field by
// position: FALSE when a pair of fields differs, otherwise NULL when a field is NULL. DuckDB's
// composite equality takes NULL fields, and NaN, as equal values. INTERVAL fields compare their
// lengths, as bq_interval_key computes them.
std::string Equal(const googlesql::Type* type, const std::string& left, const std::string& right) {
  if (type->IsStruct()) {
    std::vector<std::string> fields;
    for (int i = 0; i < type->AsStruct()->num_fields(); ++i) {
      const auto field = [&](const std::string& value) {
        return "struct_extract_at(" + value + ", " + std::to_string(i + 1) + ")";
      };
      fields.push_back(Equal(type->AsStruct()->field(i).type, field(left), field(right)));
    }
    return "(" + Join(fields, " AND ") + ")";
  }
  if (type->IsDouble()) {
    return FloatCompare(left, "=", right);
  }
  if (type->IsInterval()) {
    return "(bq_interval_key(" + left + ") = bq_interval_key(" + right + "))";
  }
  return "(" + left + " = " + right + ")";
}

// `body` over the arguments `args` that `indexes` name, each bound once as _sc.aN.
std::string BindComparison(const std::vector<std::string>& args, const std::vector<size_t>& indexes,
                           const std::string& body) {
  std::vector<std::string> fields;
  std::ranges::transform(indexes, std::back_inserter(fields), [&](size_t i) {
    return "a" + std::to_string(i + 1) + " := " + args.at(i);
  });
  return "list_transform([struct_pack(" + Join(fields, ", ") + ")], lambda _sc: " + body + ")[1]";
}

std::string BoundComparison(size_t i) { return "_sc.a" + std::to_string(i + 1); }

std::string StructInArray(const googlesql::Type* type, const std::string& value,
                          const std::string& array) {
  // TRUE when an element is equal, otherwise NULL when a comparison is NULL; FALSE for an empty
  // or NULL array.
  return BindComparison(
      {value, array}, {0, 1},
      "list_transform([list_transform(_sc.a2, lambda _e: " + Equal(type, "_sc.a1", "_e") +
          ")], lambda _b: CASE WHEN list_contains(_b, true) THEN true WHEN len(_b) > "
          "list_count(_b) THEN NULL ELSE false END)[1]");
}

}  // namespace bigquery_emulator_duckdb::function_catalog
