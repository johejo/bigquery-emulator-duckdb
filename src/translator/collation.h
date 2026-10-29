#pragma once

#include <optional>
#include <string>
#include <vector>

#include "googlesql/public/type.h"
#include "src/translator/context.h"

// Declared rather than included: the resolved AST's headers, and the serialization protos that
// resolved_collation.h includes, would otherwise be parsed by every source that includes this.
namespace googlesql {
class AnnotationMap;
class ResolvedCollation;
}  // namespace googlesql

// Collations, in collation.cc. GoogleSQL's resolver propagates collations through expressions
// as type annotations and records the collation each operation uses in the resolved AST, such as
// a function call's collation_list or an ORDER BY item's collation; the translator reads them
// only through these functions. An operation applies its collation by comparing, ordering or
// grouping the strings' sort keys in place of the strings, or by calling GoogleSQL's collated
// implementation of a function. An operation that does neither rejects a non-empty collation.
// third_party/bigquery_docs/reference/standard-sql/collation-concepts.md
namespace bigquery_emulator_duckdb::translator {

// A resolver-provided collation tree. Empty subtrees leave their values unchanged.
struct Collation {
  std::string specification;
  std::vector<Collation> children;
  [[nodiscard]] bool empty() const { return specification.empty() && children.empty(); }
};

// The collation an operation uses. The backend validates scalar specifications.
std::optional<Collation> OperationCollation(const googlesql::ResolvedCollation& collation,
                                            const Scope& scope);

// The same for a function call's collation_list, which holds at most one collation.
std::optional<Collation> OperationCollation(
    const std::vector<googlesql::ResolvedCollation>& collations, const Scope& scope);

// Replace collated STRING leaves with sort keys, preserving the surrounding ARRAY/STRUCT
// shape and NULL containers. Uncollated subtrees retain their values. `type` describes `sql`.
std::string CollationKey(const std::string& sql, const Collation& collation,
                         const googlesql::Type* type);

// Whether `annotations`, of an expression or column, hold only collations, which do not change
// the value, so that its value translates as one without them.
bool OnlyCollations(const googlesql::AnnotationMap* annotations);

// The collation tree of a column with `annotations` whose operation the resolved AST
// does not record, such as a set operation's, which compares the column under its own collation;
// including collations of ARRAY elements and STRUCT fields.
Collation ColumnCollation(const googlesql::AnnotationMap* annotations);

// Whether `annotations` hold a non-empty collation at any depth. Operations whose collation the
// resolved AST does not record can use this to detect annotated values.
bool HasCollation(const googlesql::AnnotationMap* annotations);

}  // namespace bigquery_emulator_duckdb::translator
