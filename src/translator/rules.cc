// Translates calls with the registry of src/translator/functions.cc: expands the rules of a
// function into DuckDB SQL, and spells the arguments of an aggregate rule.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/translator/functions.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

// The argument a $n, #n or !n at spelling[i] refers to, counted from 0; for !n, the error.
std::optional<std::size_t> Placeholder(std::string_view spelling, std::size_t i) {
  if ((spelling[i] != '$' && spelling[i] != '#' && spelling[i] != '!') ||
      i + 1 >= spelling.size() || spelling[i + 1] < '1' || spelling[i + 1] > '9') {
    return std::nullopt;
  }
  return static_cast<std::size_t>(spelling[i + 1] - '1');
}

bool Holds(const Condition& condition, const std::vector<FunctionArgument>& arguments) {
  if (condition.argument > arguments.size()) {
    return true;
  }
  const FunctionArgument& argument = arguments[condition.argument - 1];
  if (!condition.types.empty() &&
      std::ranges::find(condition.types, argument.type) == condition.types.end()) {
    return false;
  }
  if (!condition.date_parts.empty() &&
      (!argument.date_part || std::ranges::find(condition.date_parts, *argument.date_part) ==
                                  condition.date_parts.end())) {
    return false;
  }
  return !condition.rounding_mode || argument.rounding_mode == *condition.rounding_mode;
}

bool Matches(const Rule& rule, const std::vector<FunctionArgument>& arguments) {
  if (arguments.size() < rule.arity.min || arguments.size() > rule.arity.max) {
    return false;
  }
  for (std::size_t i = 0; i < rule.spelling.size(); ++i) {
    if (const auto index = Placeholder(rule.spelling, i);
        index && rule.spelling[i] == '#' &&
        (*index >= arguments.size() || !arguments[*index].date_part)) {
      return false;
    }
  }
  return std::ranges::all_of(
      rule.conditions, [&](const Condition& condition) { return Holds(condition, arguments); });
}

// SQL that is as cheap and as stable to repeat as a reference to it: a column, a number or a
// string without quotes in it, possibly negated or cast to a type such as BIGINT.
bool Trivial(std::string_view sql) {
  constexpr std::string_view kCast = "CAST(";
  if (sql.starts_with(kCast) && sql.ends_with(")")) {
    const std::string_view inner = sql.substr(kCast.size(), sql.size() - kCast.size() - 1);
    const std::size_t as = inner.rfind(" AS ");
    return as != std::string_view::npos && Trivial(inner.substr(0, as)) &&
           Trivial(inner.substr(as + 4));
  }
  if (sql.size() >= 2 && sql.front() == '-' && sql[1] != '-') {
    sql.remove_prefix(1);
  }
  if (sql.size() >= 2 && sql.front() == '\'' && sql.back() == '\'') {
    return sql.find('\'', 1) == sql.size() - 1;
  }
  return !sql.empty() && std::ranges::all_of(sql, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '"';
  });
}

// The rule's spelling with each !n replaced by the SQL raising error n.
std::string WithErrors(const Rule& rule, bool safe) {
  std::string spelling;
  for (std::size_t i = 0; i < rule.spelling.size(); ++i) {
    const auto index = Placeholder(rule.spelling, i);
    if (index && rule.spelling[i] == '!') {
      spelling += Raise(rule.errors.at(*index), safe);
      ++i;
    } else {
      spelling += rule.spelling[i];
    }
  }
  return spelling;
}

std::string Expand(const Rule& rule, const std::vector<FunctionArgument>& arguments, bool safe) {
  const std::string spelling = WithErrors(rule, safe);
  std::vector<std::string> sql;
  sql.reserve(std::max<std::size_t>(arguments.size(), rule.arity.max));
  for (const FunctionArgument& argument : arguments) {
    sql.push_back(argument.sql);
  }
  // An optional argument without a default must not appear in the spelling.
  for (std::size_t i = arguments.size(); i - rule.arity.min < rule.defaults.size(); ++i) {
    sql.emplace_back(rule.defaults[i - rule.arity.min]);
  }
  std::vector<int> uses(sql.size());
  for (std::size_t i = 0; i < spelling.size(); ++i) {
    if (const auto index = Placeholder(spelling, i); index && spelling[i] == '$') {
      ++uses.at(*index);
    }
  }
  // Bind the arguments once when one would otherwise be evaluated twice, keeping the call an
  // expression that CASE and IF can short-circuit.
  bool bind = false;
  for (std::size_t i = 0; i < sql.size(); ++i) {
    bind = bind || (uses[i] > 1 && !Trivial(sql[i]));
  }
  std::vector<std::string> references = sql;
  std::string fields;
  if (bind) {
    // Trivial arguments stay as they are, so a literal that DuckDB needs as a constant, such as a
    // rounding precision, remains one.
    for (std::size_t i = 0; i < sql.size(); ++i) {
      if (uses[i] == 0 || Trivial(sql[i])) {
        continue;
      }
      const std::string field = "a" + std::to_string(i + 1);
      fields += (fields.empty() ? "" : ", ") + field + " := " + sql[i];
      references[i] = "_fn." + field;
    }
  }
  std::string body;
  for (std::size_t i = 0; i < spelling.size(); ++i) {
    if (const auto index = Placeholder(spelling, i)) {
      body += spelling[i] == '$' ? references.at(*index) : sql.at(*index);
      ++i;
    } else {
      body += spelling[i];
    }
  }
  if (!bind) {
    return body;
  }
  return "list_transform([struct_pack(" + fields + ")], _fn -> " + body + ")[1]";
}

std::string Invoke(std::string_view function, const std::vector<FunctionArgument>& arguments) {
  std::string sql = std::string(function) + "(";
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    sql += (i == 0 ? "" : ", ") + arguments[i].sql;
  }
  return sql + ")";
}

// Substitutes the translated arguments for each $n in `spelling`.
std::string Substitute(std::string_view spelling, const std::vector<std::string>& arguments) {
  std::string sql;
  for (std::size_t i = 0; i < spelling.size(); ++i) {
    if (const auto index = Placeholder(spelling, i); index && spelling[i] == '$') {
      sql += arguments.at(*index);
      ++i;
    } else {
      sql += spelling[i];
    }
  }
  return sql;
}

}  // namespace

std::optional<std::string> TranslateFunction(std::string_view upper_name,
                                             const std::vector<FunctionArgument>& arguments,
                                             bool safe) {
  const FunctionEntry* entry = FindFunction(upper_name);
  if (entry == nullptr) {
    return std::nullopt;
  }
  switch (entry->implementation) {
    case Implementation::kSame:
      return Invoke(upper_name, arguments);
    case Implementation::kRenamed:
      return Invoke(entry->target, arguments);
    case Implementation::kRules:
    case Implementation::kBackend:
      // A function with rules is translated by them alone, so a call no rule matches, such as
      // one with an argument type the rules leave out, stays unsupported.
      for (const Rule& rule : entry->rules) {
        if (Matches(rule, arguments)) {
          return Expand(rule, arguments, safe);
        }
      }
      return std::nullopt;
    default:
      return std::nullopt;
  }
}

std::vector<std::string> AggregateArguments(const AggregateRule& rule,
                                            const std::vector<std::string>& arguments) {
  if (rule.arguments.empty()) {
    return arguments;
  }
  std::vector<std::string> given = arguments;
  for (std::size_t i = given.size(); i < rule.defaults.size(); ++i) {
    given.emplace_back(rule.defaults[i]);
  }
  std::vector<std::string> sql;
  sql.reserve(rule.arguments.size());
  for (const std::string_view spelling : rule.arguments) {
    sql.push_back(Substitute(spelling, given));
  }
  return sql;
}

}  // namespace bigquery_emulator_duckdb::translator
