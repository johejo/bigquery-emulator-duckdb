#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/function_catalog/rules.h"
#include "src/timestamp_precision.h"

namespace googlesql {
class ResolvedExpr;
class ResolvedFunctionCall;
class Type;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::function_catalog {

// A scalar call that a handler translates.
struct ScalarCall {
  const googlesql::ResolvedFunctionCall& resolved;
  // The function name in upper case, as the registry has it.
  std::string_view name;
  std::vector<FunctionArgument> arguments = {};
  bool safe = false;
  // The project the query runs in, which connection IDs without one belong to.
  std::string_view project = {};
  // The precision of the TIMESTAMPs in `arguments`' SQL and in the SQL a handler returns:
  // microseconds when Call adapts a nanosecond query to a microsecond overload, so it can
  // differ from the catalog's timestamp_precision().
  TimestampPrecision sql_timestamp_precision;

  // Access resolved arguments and types without including the AST definitions in handlers.
  [[nodiscard]] const googlesql::ResolvedExpr& Argument(std::size_t i) const;
  [[nodiscard]] const googlesql::Type* ArgumentType(std::size_t i) const;
  [[nodiscard]] const googlesql::Type* ResultType() const;

  [[nodiscard]] std::string Raise(std::string_view message) const {
    return function_catalog::Raise(message, safe);
  }
};

// How the translator dispatches a BigQuery function.
enum class Translation : std::uint8_t {
  // The DuckDB function of the same name, with the same arguments.
  kSame,
  // A DuckDB function of another name, with the same arguments.
  kRenamed,
  // DuckDB SQL templates, chosen by the arguments.
  kRules,
  // A scalar handler supplied when assembling the catalog.
  kHandler,
  // Another function with the SAFE. prefix, as SAFE_ADD is SAFE.$ADD.
  kSafe,
  // A DuckDB aggregate function, also usable over a window.
  kAggregate,
  // A DuckDB window function.
  kAnalytic,
};

// A function in the registry.
struct FunctionEntry {
  Translation translation;
  TypeUses uses = {};
  std::vector<Implementation> implementations;
  // kRules: the rules, the first that matches a call translating it.
  std::vector<Rule> rules = {};
  // kRenamed: the DuckDB function. kSafe: the function it is with SAFE.
  std::string_view target = {};
  // kHandler.
  Handler handler = nullptr;
  // By argument position; positions past the end use kExpression.
  std::vector<ArgumentMode> argument_modes = {};
  // Overrides the usual translation when the first argument is a STRUCT.
  Handler struct_handler = nullptr;
  std::vector<TypeRejection> type_rejections = {};
  // kAggregate and kAnalytic: the overloads, the first that matches a call translating it.
  std::vector<AggregateRule> aggregates = {};
};

// Which forms the translator rejects that GoogleSQL accepts but BigQuery does not. Defaults keep
// GoogleSQL's forms.
struct QueryRestrictions {
  // JSON_KEYS takes no SAFE. prefix.
  bool json_keys_rejects_safe = false;
  // INTERSECT and EXCEPT take only DISTINCT.
  bool intersect_except_distinct_only = false;
  // Window PARTITION BY takes no STRUCT or ARRAY keys.
  bool scalar_window_partition_keys_only = false;
  // DISTINCT set operations take no STRUCT columns.
  bool distinct_set_operations_without_structs = false;
  // DISTINCT set operations take no ARRAY columns.
  bool distinct_set_operations_without_arrays = false;
  // IS DISTINCT FROM and IS NOT DISTINCT FROM take no STRUCT arguments.
  bool distinct_comparisons_without_structs = false;
  // Query results and written values hold no ARRAY with a NULL element.
  bool arrays_without_null_elements = false;
  // Query results take no empty STRUCTs or anonymous fields of STRUCT value tables.
  bool restrict_struct_results = false;
};

// A catalog owns its entries; registered names and targets must outlive it.
// The application assembles a catalog before passing it to the translator.
class Catalog {
 public:
  Catalog(TimestampPrecision precision, QueryRestrictions restrictions)
      : timestamp_precision_(precision), restrictions_(restrictions) {}
  [[nodiscard]] TimestampPrecision timestamp_precision() const { return timestamp_precision_; }
  [[nodiscard]] const QueryRestrictions& restrictions() const { return restrictions_; }
  void Add(std::string_view name, FunctionEntry entry);
  // Composition omits service-dependent forms whose backend capability is absent.
  void Remove(std::string_view name);
  void AddRules(std::string_view name, FunctionDefinition<Rule> definition,
                Implementation fallback);
  void AddRules(std::string_view name, const StaticFunctionDefinition<StaticRule>& definition,
                Implementation fallback);
  void AddAggregates(std::string_view name,
                     const StaticFunctionDefinition<StaticAggregateRule>& definition,
                     Translation translation, std::optional<Implementation> backend = {});
  [[nodiscard]] const FunctionEntry* Find(std::string_view upper_name) const;
  [[nodiscard]] bool HasTypeUse(std::string_view upper_name, TypeUse use) const;
  [[nodiscard]] bool SupportsCollation(std::string_view upper_name) const;
  [[nodiscard]] std::optional<std::string> Translate(std::string_view upper_name,
                                                     const std::vector<FunctionArgument>& arguments,
                                                     bool safe = false,
                                                     std::string_view collation = {}) const;

 private:
  TimestampPrecision timestamp_precision_;
  QueryRestrictions restrictions_;
  std::unordered_map<std::string_view, FunctionEntry> functions_;
};

// The implementation descriptions used by the function support report.
std::string Describe(const FunctionEntry& entry);
std::vector<std::string> AggregateArguments(const AggregateRule& rule,
                                            const std::vector<std::string>& arguments);
std::string AggregateOrder(const AggregateRule& rule, const std::vector<std::string>& arguments);

}  // namespace bigquery_emulator_duckdb::function_catalog
