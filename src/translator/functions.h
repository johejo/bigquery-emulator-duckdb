#pragma once

// The registry of the BigQuery functions the translator supports. Each function is declared once,
// in src/translator/functions.cc, with how it is implemented.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "googlesql/public/type.pb.h"

namespace googlesql {
class ResolvedFunctionCall;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {

// An argument of a BigQuery function call, as far as the translation rules look at it.
struct FunctionArgument {
  // The argument translated to DuckDB SQL.
  std::string sql;
  googlesql::TypeKind type = googlesql::TYPE_UNKNOWN;
  // The date part, such as "day", when the argument is one.
  std::optional<std::string> date_part;
  // The rounding mode, such as "ROUND_HALF_EVEN", when the argument is one.
  std::optional<std::string> rounding_mode;
  // The value of a STRING literal argument.
  std::optional<std::string> string_literal;
};

// The SQL that raises the error whose message is the DuckDB string expression `message`, or NULL
// when the call has the SAFE. prefix, which turns the function's own errors into NULL.
inline std::string Raise(std::string_view message, bool safe) {
  return safe ? "NULL" : "error(" + std::string(message) + ")";
}

// A scalar call that a handler translates.
struct ScalarCall {
  const googlesql::ResolvedFunctionCall& resolved;
  // The function name in upper case, as the registry has it.
  std::string_view name;
  std::vector<FunctionArgument> arguments;
  bool safe = false;

  std::string Raise(std::string_view message) const { return translator::Raise(message, safe); }
};

// Translates a call that templates cannot express, or returns nullopt when it is unsupported.
using Handler = std::optional<std::string> (*)(const ScalarCall&);

// How the emulator implements a BigQuery function.
enum class Implementation : std::uint8_t {
  // The DuckDB function of the same name, with the same arguments.
  kSame,
  // A DuckDB function of another name, with the same arguments.
  kRenamed,
  // DuckDB SQL templates, chosen by the arguments.
  kRules,
  // Templates calling GoogleSQL's own implementation, which src/backend_functions.cc registers
  // with DuckDB as a bq_* function.
  kBackend,
  // Code in src/translator/function.cc.
  kHandler,
  // Another function with the SAFE. prefix, as SAFE_ADD is SAFE.$ADD.
  kSafe,
  // A DuckDB aggregate function, also usable over a window.
  kAggregate,
  // A DuckDB window function.
  kAnalytic,
};

// How docs/functions.md names an implementation.
std::string_view Describe(Implementation implementation);

// The argument counts a rule accepts.
struct Arity {
  // NOLINTNEXTLINE(google-explicit-constructor): a bare count reads best in the table.
  Arity(std::size_t count) : min(count), max(count) {}
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the order reads as a range.
  Arity(std::size_t min, std::size_t max) : min(min), max(max) {}

  std::size_t min;
  std::size_t max;
};

// What argument `argument` (counted from 1) has to be for a rule to apply. A condition on an
// optional argument the call leaves out holds.
struct Condition {
  std::size_t argument;
  // Any of these types; empty accepts any type.
  std::vector<googlesql::TypeKind> types;
  // Any of these date parts; empty accepts any argument.
  std::vector<std::string_view> date_parts;
  // This rounding mode.
  std::optional<std::string_view> rounding_mode;
  // A STRING literal with this value.
  std::optional<std::string_view> string_literal;
};

// A DuckDB spelling of a BigQuery function. In the spelling, $n is argument n, #n argument n,
// which has to be a date part, as a lower case string literal, and !n raises error n. An argument
// that the spelling uses more than once is evaluated once, so volatile arguments stay consistent.
struct Rule {
  Arity arity;
  std::string spelling;
  std::vector<Condition> conditions = {};
  // The spellings of the optional arguments, from the first one past arity.min on.
  std::vector<std::string_view> defaults = {};
  // The messages of the errors the spelling raises, as DuckDB string expressions that may use
  // the arguments. Under SAFE. the errors are NULL instead.
  std::vector<std::string> errors = {};
};

// A DuckDB aggregate or window function that implements a BigQuery one.
struct AggregateRule {
  // How the function treats IGNORE NULLS and RESPECT NULLS.
  enum class Nulls : std::uint8_t {
    // It respects NULLs, and IGNORE NULLS is unsupported.
    kRespect,
    // IGNORE NULLS leaves out the rows whose first argument is NULL.
    kFilter,
    // DuckDB takes IGNORE NULLS too.
    kModifier,
    // It always ignores NULLs, and RESPECT NULLS is unsupported.
    kIgnore,
  };
  enum class Distinct : std::uint8_t { kAllowed, kAlways, kUnsupported };
  // How the function takes LIMIT.
  enum class Limit : std::uint8_t {
    kUnsupported,
    // The first elements of the list it builds.
    kSlice,
    // The first non-NULL values of the list STRING_AGG joins.
    kJoin,
  };

  std::string_view function;
  // The type the first argument has to have; TYPE_UNKNOWN accepts any.
  googlesql::TypeKind type = googlesql::TYPE_UNKNOWN;
  // The DuckDB arguments, with $n for argument n, when they differ from the BigQuery ones.
  std::vector<std::string_view> arguments = {};
  // The spellings of the arguments a call leaves out, by position; empty for none.
  std::vector<std::string_view> defaults = {};
  Nulls nulls = Nulls::kRespect;
  Distinct distinct = Distinct::kAllowed;
  Limit limit = Limit::kUnsupported;
  // Leaves out the rows whose first argument is NULL.
  bool skip_nulls = false;
  // Turns the DuckDB call `sql` into the BigQuery result, given the BigQuery arguments and the
  // FILTER and OVER clauses of the call; null keeps it as it is.
  std::string (*finish)(const std::string& sql, const std::vector<std::string>& arguments,
                        const std::string& tail) = nullptr;
};

// A function in the registry.
struct FunctionEntry {
  Implementation implementation;
  // kRules and kBackend: the rules, the first that matches a call translating it.
  std::vector<Rule> rules = {};
  // kRenamed: the DuckDB function. kSafe: the function it is with SAFE.
  std::string_view target = {};
  // kHandler.
  Handler handler = nullptr;
  // kAggregate and kAnalytic: the overloads, the first that matches a call translating it.
  std::vector<AggregateRule> aggregates = {};
};

// The registry entry of `upper_name`, the function name in upper case, or null when the
// emulator does not support it. The operators have their internal names, such as $ADD.
const FunctionEntry* FindFunction(std::string_view upper_name);

// The DuckDB spelling of a call to `upper_name`, a function implemented by kSame, kRenamed,
// kRules or kBackend, or nullopt when no rule matches.
std::optional<std::string> TranslateFunction(std::string_view upper_name,
                                             const std::vector<FunctionArgument>& arguments,
                                             bool safe = false);

// The DuckDB arguments of a call to an aggregate rule, given the translated BigQuery ones.
std::vector<std::string> AggregateArguments(const AggregateRule& rule,
                                            const std::vector<std::string>& arguments);

}  // namespace bigquery_emulator_duckdb::translator
