#pragma once

// Rule definitions shared by the translator and feature-owned backend registrations.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "googlesql/public/type.pb.h"

namespace bigquery_emulator_duckdb::function_catalog {

// An argument of a BigQuery function call, as far as the translation rules look at it.
struct FunctionArgument {
  // The argument translated to DuckDB SQL.
  std::string sql;
  googlesql::TypeKind type = googlesql::TYPE_UNKNOWN;
  // The date part, such as "day", when the argument is one.
  std::optional<std::string> date_part = {};
  // The rounding mode, such as "ROUND_HALF_EVEN", when the argument is one.
  std::optional<std::string> rounding_mode = {};
};

// The SQL that raises the error whose message is the DuckDB string expression `message`, or NULL
// when the call has the SAFE. prefix, which turns the function's own errors into NULL.
inline std::string Raise(std::string_view message, bool safe) {
  return safe ? "NULL" : "error(" + std::string(message) + ")";
}

// The implementation shown in docs/functions.md, independent of how a call is translated.
enum class Implementation : std::uint8_t {
  kSame,
  kRenamed,
  kSQL,
  kSQLInCode,
  kGoogleSQL,
  kS2,
  kCustom,
  kSafe,
  kAggregate,
  kAnalytic,
};

// The argument counts a rule accepts.
struct Arity {
  // A bare count reads best in the table.
  // cppcheck-suppress noExplicitConstructor
  // NOLINTNEXTLINE(google-explicit-constructor,cppcoreguidelines-explicit-constructor,misc-explicit-constructor)
  constexpr Arity(std::size_t count) : min(count), max(count) {}
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the order reads as a range.
  constexpr Arity(std::size_t min, std::size_t max) : min(min), max(max) {}

  std::size_t min;
  std::size_t max;
};

// Owning rules are built at runtime; static rules only refer to immutable backing arrays.
template <typename T>
using OwnedRuleList = std::vector<T>;
template <typename T>
using StaticRuleList = std::span<const T>;

// What argument `argument` (counted from 1) has to be for a rule to apply. A condition on an
// optional argument the call leaves out holds.
template <template <typename> typename List>
struct BasicCondition {
  std::size_t argument = 0;
  // Any of these types; empty accepts any type.
  List<googlesql::TypeKind> types = {};
  // Any of these date parts; empty accepts any argument.
  List<std::string_view> date_parts = {};
  // This rounding mode.
  std::optional<std::string_view> rounding_mode = {};
};

using Condition = BasicCondition<OwnedRuleList>;
using StaticCondition = BasicCondition<StaticRuleList>;

// A DuckDB spelling of a BigQuery function. In the spelling, $n is argument n, #n argument n,
// which has to be a date part, as a lower case string literal, and !n raises error n. An argument
// that the spelling uses more than once is evaluated once, so volatile arguments stay consistent.
template <typename Text, template <typename> typename List>
struct BasicRule {
  Arity arity = 0;
  Text spelling;
  List<BasicCondition<List>> conditions = {};
  // The spellings of the optional arguments, from the first one past arity.min on.
  List<std::string_view> defaults = {};
  // The messages of the errors the spelling raises, as DuckDB string expressions that may use
  // the arguments. Under SAFE. the errors are NULL instead.
  List<Text> errors = {};
  // Overrides the rule table's implementation when this overload uses another backend.
  std::optional<Implementation> implementation = {};
};

using Rule = BasicRule<std::string, OwnedRuleList>;
using StaticRule = BasicRule<std::string_view, StaticRuleList>;

Rule Own(const StaticRule& rule);

// An aggregate or analytic argument that BigQuery requires to be a constant expression.
struct ConstantArgument {
  // Counted from 1; a call that leaves this optional argument out satisfies the requirement.
  std::size_t argument;
  std::string_view description;
  std::string_view message;
};

// A DuckDB aggregate or window function that implements a BigQuery one.
struct AggregateOptions {
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
    // It leaves out the rows whose first argument is NULL unless the call has RESPECT NULLS.
    kFilterUnlessRespected,
  };
  enum class Distinct : std::uint8_t {
    kAllowed,
    kAlways,
    kUnsupported,
    // Only a call with DISTINCT matches the rule, whose function or finish removes the duplicates.
    kRequired,
  };
  // How the function takes LIMIT.
  enum class Limit : std::uint8_t {
    kUnsupported,
    // The first elements of the list it builds.
    kSlice,
    // The first elements of the list its finish returns.
    kSliceFinished,
    // The first non-NULL values of the list STRING_AGG joins.
    kJoin,
  };
};

template <template <typename> typename List>
struct BasicAggregateRule : AggregateOptions {
  std::string_view function;
  // The type the first argument has to have; TYPE_UNKNOWN accepts any.
  googlesql::TypeKind type = googlesql::TYPE_UNKNOWN;
  // What the other arguments have to be; only their types count.
  List<BasicCondition<List>> conditions = {};
  // The DuckDB arguments, with $n for argument n, when they differ from the BigQuery ones.
  List<std::string_view> arguments = {};
  // The order of the DuckDB call, with $n for argument n, when the BigQuery call has none.
  std::string_view order = {};
  // The spellings of the arguments a call leaves out, by position; empty for none.
  List<std::string_view> defaults = {};
  Nulls nulls = Nulls::kRespect;
  Distinct distinct = Distinct::kAllowed;
  Limit limit = Limit::kUnsupported;
  // Leaves out the rows whose first argument is NULL.
  bool skip_nulls = false;
  // Passes or collects the values as they are, so they may hold INTERVAL values, except with
  // DISTINCT, which would compare them as DuckDB does; the analyzer rejects DISTINCT over them.
  bool carries = false;
  // A call with an operation collation matches only the rules that set this, whose $n count on
  // past the call's arguments to their collation keys: with one argument, $2 is the key of $1.
  // With Distinct::kRequired, the function collects the first argument, and
  // bq_collation_distinct leaves out its equal values and sorts them by the call's ORDER BY,
  // which DISTINCT allows only over that argument, before LIMIT and the finish.
  bool collated = false;
  // Turns the DuckDB call `sql` into the BigQuery result, given the BigQuery arguments and the
  // FILTER and OVER clauses of the call; null keeps it as it is.
  std::string (*finish)(const std::string& sql, const std::vector<std::string>& arguments,
                        const std::string& tail) = nullptr;
  // A finish containing a subquery needs its aggregate result in a preceding SELECT.
  // DuckDB cannot bind constant aggregates or correlated windows inside that subquery.
  bool finish_after_scan = false;
  // Reject ORDER BY of the first argument alone, or any ORDER BY with DISTINCT, with this
  // error. Other order keys, including ones combined with the first argument, remain allowed.
  std::string_view order_by_argument_error = {};
  List<ConstantArgument> constant_arguments = {};
};

using AggregateRule = BasicAggregateRule<OwnedRuleList>;
using StaticAggregateRule = BasicAggregateRule<StaticRuleList>;

// Copy a static definition into catalog-owned storage at the registration boundary.
AggregateRule Own(const StaticAggregateRule& rule);

// What functions do with values of a type that DuckDB holds in another type, such as a RANGE in a
// STRUCT, and other per-function differences from GoogleSQL. A type takes the uses whose
// operations DuckDB performs on its stand-in as BigQuery does on its values; other functions
// would see the stand-in.
enum class TypeUse : std::uint8_t {
  // Handle graph values without exposing their private STRUCT metadata.
  kGraphValues,
  // Handle BIGNUMERIC arguments and results without loss of precision. Other DuckDB functions
  // could compute BIGNUM values as DOUBLE or truncate them.
  kBigNumeric,
  // Pass, select or count their values without reading them.
  kCarries,
  // Carry result arguments when the first, compared argument contains no INTERVAL values.
  kCarriesResults,
  // Also compare or order their values.
  kCompares,
  // Build or read DuckDB intervals, which hold INTERVAL values.
  kBuildsOrReadsInterval,
  // Only compare their INTERVAL arguments or ARRAY elements, so they compare the values' keys.
  kComparesIntervalKeys,
  // Only compare their STRING arguments, so that under an operation collation they compare the
  // arguments' collation keys.
  kComparesCollationKeys,
  // Compare under an operation collation while returning original values, not their keys.
  kSelectsCollationValues,
  // STRUCT handlers that also compare INTERVAL fields, by their keys. BigQuery
  // compares no ARRAYs, also in STRUCT fields.
  kComparesStructsWithInterval,
  // Build or read the STRUCT that holds a RANGE, whose unbounded ends are infinite, or read it as
  // a RANGE in the JSON and FORMAT backends. DuckDB carries, compares and orders it as BigQuery
  // does a RANGE.
  kBuildsOrReadsRange,
  // Aggregate or analytic functions that order, compare or carry a RANGE's STRUCT as BigQuery
  // does its RANGE.
  kAggregatesRange,
  // Build or read the BLOB that holds a GEOGRAPHY without treating it as BYTES.
  kBuildsOrReadsGeography,
  // Read the placeholder field of a STRUCT<> as an empty STRUCT, as the JSON and FORMAT backends
  // do, or carry it.
  kReadsEmptyStruct,
  // BigQuery rejects the SAFE. prefix on these although GoogleSQL accepts it.
  kRejectsSafe,
  // BigQuery rejects an operation collation on these although GoogleSQL applies it.
  kRejectsCollation,
  // BigQuery rejects OVER on these aggregate functions although GoogleSQL accepts it.
  kRejectsOver,
  // BigQuery rejects DISTINCT on these aggregate functions although GoogleSQL accepts it.
  kRejectsDistinct,
  // BigQuery rejects DISTINCT on these analytic functions although GoogleSQL accepts it.
  kRejectsAnalyticDistinct,
};

// A constexpr set of type attributes, also used by the plain and handler registrations.
class TypeUses {
 public:
  constexpr TypeUses() = default;
  constexpr TypeUses(std::initializer_list<TypeUse> uses)
      : bits_(std::accumulate(uses.begin(), uses.end(), std::uint32_t{0},
                              [](std::uint32_t bits, TypeUse use) {
                                return bits | (std::uint32_t{1} << static_cast<unsigned>(use));
                              })) {}

  constexpr TypeUses& operator|=(TypeUses other) {
    bits_ |= other.bits_;
    return *this;
  }

  [[nodiscard]] constexpr bool Contains(TypeUse use) const {
    return (bits_ & (std::uint32_t{1} << static_cast<unsigned>(use))) != 0;
  }

 private:
  std::uint32_t bits_ = 0;
};

// How an argument is translated before the function's implementation receives it.
enum class ArgumentMode : std::uint8_t {
  kExpression,
  // Accept a single-part INTERVAL width and pass its count of months, days or microseconds.
  kBucketWidth,
  // Compare INTERVAL array elements by their keys, leaving other element types unchanged.
  kIntervalArrayKeys,
};

// A type combination BigQuery rejects after GoogleSQL analysis. All listed argument positions
// must be present and match; TYPE_UNKNOWN accepts any type. The type list must outlive the catalog.
struct TypeRejection {
  std::span<const googlesql::TypeKind> types;
  std::string_view message;
};

struct ScalarCall;

// Translates a call that templates cannot express, or returns nullopt when it is unsupported.
using Handler = std::optional<std::string> (*)(const ScalarCall&);

// A function's overloads and type behavior, declared together in its rule table.
template <typename RuleType, template <typename> typename List = OwnedRuleList>
struct FunctionDefinition {
  List<RuleType> rules = {};
  TypeUses uses = {};
  List<ArgumentMode> argument_modes = {};
  // Overrides the usual translation when the first argument is a STRUCT.
  Handler struct_handler = nullptr;
  List<TypeRejection> type_rejections = {};
};

// All backing arrays must outlive the definition and any catalog using its string views.
template <typename RuleType>
using StaticFunctionDefinition = FunctionDefinition<RuleType, StaticRuleList>;

// Definition lists are enumerated during registration, not searched by name.
template <typename RuleType, template <typename> typename List = OwnedRuleList>
using NamedDefinition = std::pair<std::string_view, FunctionDefinition<RuleType, List>>;

template <typename RuleType>
using StaticNamedDefinition = NamedDefinition<RuleType, StaticRuleList>;

}  // namespace bigquery_emulator_duckdb::function_catalog
