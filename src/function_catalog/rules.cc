// Expands the supplied catalog's rules into DuckDB SQL and spells aggregate arguments.

#include "src/function_catalog/rules.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/duckdb_sql.h"
#include "src/function_catalog/catalog.h"

namespace bigquery_emulator_duckdb::function_catalog {
namespace {

// The argument a $n, #n or !n at spelling[i] refers to, counted from 0; for !n, the error.
std::optional<std::size_t> Placeholder(std::string_view spelling, std::size_t i) {
  if ((spelling.at(i) != '$' && spelling.at(i) != '#' && spelling.at(i) != '!') ||
      i + 1 >= spelling.size() || spelling.at(i + 1) < '1' || spelling.at(i + 1) > '9') {
    return std::nullopt;
  }
  return static_cast<std::size_t>(spelling.at(i + 1) - '1');
}

bool Holds(const Condition& condition, const std::vector<FunctionArgument>& arguments) {
  if (condition.argument > arguments.size()) {
    return true;
  }
  const FunctionArgument& argument = arguments.at(condition.argument - 1);
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
        index && rule.spelling.at(i) == '#' &&
        (*index >= arguments.size() || !arguments.at(*index).date_part)) {
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
  if (sql.starts_with(kCast) && sql.ends_with(')')) {
    const std::string_view inner = sql.substr(kCast.size(), sql.size() - kCast.size() - 1);
    const std::size_t as = inner.rfind(" AS ");
    return as != std::string_view::npos && Trivial(inner.substr(0, as)) &&
           Trivial(inner.substr(as + 4));
  }
  if (sql.size() >= 2 && sql.front() == '-' && sql.at(1) != '-') {
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
std::string WithErrors(const Rule& rule, bool safe, std::string_view source) {
  std::string spelling;
  for (std::size_t i = 0; i < source.size(); ++i) {
    const auto index = Placeholder(source, i);
    if (index && source.at(i) == '!') {
      spelling += Raise(rule.errors.at(*index), safe);
      ++i;
    } else {
      spelling += source.at(i);
    }
  }
  return spelling;
}

std::string Expand(const Rule& rule, const std::vector<FunctionArgument>& arguments, bool safe,
                   std::string_view collation) {
  const std::string spelling =
      WithErrors(rule, safe, collation.empty() ? rule.spelling : rule.collated_spelling);
  std::vector<std::string> sql;
  sql.reserve(std::max<std::size_t>(arguments.size(), rule.arity.max));
  std::ranges::transform(arguments, std::back_inserter(sql), &FunctionArgument::sql);
  // An optional argument without a default must not appear in the spelling.
  for (std::size_t i = arguments.size(); i - rule.arity.min < rule.defaults.size(); ++i) {
    sql.emplace_back(rule.defaults.at(i - rule.arity.min));
  }
  if (!collation.empty()) {
    sql.resize(rule.arity.max);
    sql.push_back(QuoteLiteral(collation));
  }
  std::vector<int> uses(sql.size());
  for (std::size_t i = 0; i < spelling.size(); ++i) {
    if (const auto index = Placeholder(spelling, i); index && spelling.at(i) == '$') {
      ++uses.at(*index);
    }
  }
  // Bind the arguments once when one would otherwise be evaluated twice, keeping the call an
  // expression that CASE and IF can short-circuit.
  bool bind = false;
  for (std::size_t i = 0; i < sql.size(); ++i) {
    bind = bind || (uses.at(i) > 1 && !Trivial(sql.at(i)));
  }
  std::vector<std::string> references = sql;
  std::vector<std::string> fields;
  if (bind) {
    // Trivial arguments stay as they are, so a literal that DuckDB needs as a constant, such as a
    // rounding precision, remains one.
    for (std::size_t i = 0; i < sql.size(); ++i) {
      if (uses.at(i) == 0 || Trivial(sql.at(i))) {
        continue;
      }
      const std::string field = "a" + std::to_string(i + 1);
      fields.push_back(field + " := " + sql.at(i));
      references.at(i) = "_fn." + field;
    }
  }
  std::string body;
  for (std::size_t i = 0; i < spelling.size(); ++i) {
    if (const auto index = Placeholder(spelling, i)) {
      body += spelling.at(i) == '$' ? references.at(*index) : sql.at(*index);
      ++i;
    } else {
      body += spelling.at(i);
    }
  }
  if (!bind) {
    return body;
  }
  return "list_transform([struct_pack(" + Join(fields, ", ") + ")], lambda _fn: " + body + ")[1]";
}

std::string Invoke(std::string_view function, const std::vector<FunctionArgument>& arguments) {
  std::vector<std::string> sql;
  std::ranges::transform(arguments, std::back_inserter(sql), &FunctionArgument::sql);
  return std::string(function) + "(" + Join(sql, ", ") + ")";
}

// Substitutes the translated arguments for each $n in `spelling`.
std::string Substitute(std::string_view spelling, const std::vector<std::string>& arguments) {
  std::string sql;
  for (std::size_t i = 0; i < spelling.size(); ++i) {
    if (const auto index = Placeholder(spelling, i); index && spelling.at(i) == '$') {
      sql += arguments.at(*index);
      ++i;
    } else {
      sql += spelling.at(i);
    }
  }
  return sql;
}

}  // namespace

bool Catalog::SupportsCollation(std::string_view upper_name) const {
  const FunctionEntry* entry = Find(upper_name);
  return entry != nullptr && std::ranges::any_of(entry->rules, [](const Rule& rule) {
           return !rule.collated_spelling.empty();
         });
}

std::optional<std::string> Catalog::Translate(std::string_view upper_name,
                                              const std::vector<FunctionArgument>& arguments,
                                              bool safe, std::string_view collation) const {
  const FunctionEntry* entry = Find(upper_name);
  if (entry == nullptr) {
    return std::nullopt;
  }
  switch (entry->translation) {
    case Translation::kSame:
      return Invoke(upper_name, arguments);
    case Translation::kRenamed:
      return Invoke(entry->target, arguments);
    case Translation::kRules:
      // A function with rules is translated by them alone, so a call no rule matches, such as
      // one with an argument type the rules leave out, stays unsupported.
      if (const auto rule = std::ranges::find_if(entry->rules,
                                                 [&](const Rule& candidate) {
                                                   return (collation.empty() ||
                                                           !candidate.collated_spelling.empty()) &&
                                                          Matches(candidate, arguments);
                                                 });
          rule != entry->rules.end()) {
        return Expand(*rule, arguments, safe, collation);
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
    given.emplace_back(rule.defaults.at(i));
  }
  std::vector<std::string> sql;
  sql.reserve(rule.arguments.size());
  std::ranges::transform(rule.arguments, std::back_inserter(sql),
                         [&](std::string_view spelling) { return Substitute(spelling, given); });
  return sql;
}

std::string AggregateOrder(const AggregateRule& rule, const std::vector<std::string>& arguments) {
  return rule.order.empty() ? "" : Substitute(rule.order, arguments);
}

namespace {

std::vector<Condition> OwnConditions(std::span<const StaticCondition> conditions) {
  std::vector<Condition> result;
  result.reserve(conditions.size());
  std::ranges::transform(
      conditions, std::back_inserter(result), [](const StaticCondition& condition) {
        return Condition{
            .argument = condition.argument,
            .types = {condition.types.begin(), condition.types.end()},
            .date_parts = {condition.date_parts.begin(), condition.date_parts.end()},
            .rounding_mode = condition.rounding_mode,
        };
      });
  return result;
}

}  // namespace

Rule Own(const StaticRule& rule) {
  return {
      .arity = rule.arity,
      .spelling = std::string(rule.spelling),
      .collated_spelling = std::string(rule.collated_spelling),
      .conditions = OwnConditions(rule.conditions),
      .defaults = {rule.defaults.begin(), rule.defaults.end()},
      .errors = {rule.errors.begin(), rule.errors.end()},
      .implementation = rule.implementation,
  };
}

AggregateRule Own(const StaticAggregateRule& rule) {
  return {
      .function = rule.function,
      .type = rule.type,
      .conditions = OwnConditions(rule.conditions),
      .arguments = {rule.arguments.begin(), rule.arguments.end()},
      .order = rule.order,
      .defaults = {rule.defaults.begin(), rule.defaults.end()},
      .nulls = rule.nulls,
      .distinct = rule.distinct,
      .limit = rule.limit,
      .skip_nulls = rule.skip_nulls,
      .carries = rule.carries,
      .collated = rule.collated,
      .finish = rule.finish,
      .finish_after_scan = rule.finish_after_scan,
      .order_by_argument_error = rule.order_by_argument_error,
      .constant_arguments = {rule.constant_arguments.begin(), rule.constant_arguments.end()},
  };
}

}  // namespace bigquery_emulator_duckdb::function_catalog
