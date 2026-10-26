#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/function_catalog/rules.h"

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
  // Code in src/translator/handlers.cc.
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

// A catalog owns its entries; registered names and targets must outlive it.
// The application assembles a catalog before passing it to the translator.
class Catalog {
 public:
  void Add(std::string_view name, FunctionEntry entry);
  void AddRules(std::string_view name, FunctionDefinition<Rule> definition,
                Implementation fallback);
  void AddRules(std::string_view name, const StaticFunctionDefinition<StaticRule>& definition,
                Implementation fallback);
  void AddAggregates(std::string_view name,
                     const StaticFunctionDefinition<StaticAggregateRule>& definition,
                     Translation translation, std::optional<Implementation> backend = {});
  [[nodiscard]] const FunctionEntry* Find(std::string_view upper_name) const;
  [[nodiscard]] bool HasTypeUse(std::string_view upper_name, TypeUse use) const;
  [[nodiscard]] std::optional<std::string> Translate(std::string_view upper_name,
                                                     const std::vector<FunctionArgument>& arguments,
                                                     bool safe = false) const;

 private:
  std::unordered_map<std::string_view, FunctionEntry> functions_;
};

// The implementation descriptions used by the function support report.
std::string Describe(const FunctionEntry& entry);
std::vector<std::string> AggregateArguments(const AggregateRule& rule,
                                            const std::vector<std::string>& arguments);
std::string AggregateOrder(const AggregateRule& rule, const std::vector<std::string>& arguments);

}  // namespace bigquery_emulator_duckdb::function_catalog
