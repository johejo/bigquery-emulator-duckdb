#include "src/translator/collation.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "googlesql/public/types/annotation.h"
#include "googlesql/public/types/simple_value.h"
#include "googlesql/resolved_ast/resolved_collation.h"
#include "src/duckdb_sql.h"
#include "src/translator/context.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

constexpr int kCollation = static_cast<int>(googlesql::AnnotationKind::kCollation);

// Calls `visit` with the collation of `annotations` and of each of its fields, at any depth.
template <typename Visit>
void EachCollation(const googlesql::AnnotationMap& annotations, Visit visit) {
  if (const googlesql::SimpleValue* collation = annotations.GetAnnotation(kCollation);
      collation != nullptr && collation->has_string_value()) {
    visit(collation->string_value());
  }
  if (const auto* fields = annotations.AsStructMap(); fields != nullptr) {
    for (int i = 0; i < fields->num_fields(); ++i) {
      if (const googlesql::AnnotationMap* field = fields->field(i); field != nullptr) {
        EachCollation(*field, visit);
      }
    }
  }
}

}  // namespace

std::optional<std::string> OperationCollation(const googlesql::ResolvedCollation& collation,
                                              const Scope& scope) {
  if (collation.Empty()) {
    return "";
  }
  if (collation.num_children() > 0) {
    return Unsupported(scope, "collation of ARRAY elements or STRUCT fields");
  }
  return std::string(collation.CollationName());
}

std::optional<std::string> OperationCollation(
    const std::vector<googlesql::ResolvedCollation>& collations, const Scope& scope) {
  if (collations.empty()) {
    return "";
  }
  if (collations.size() > 1) {
    return Unsupported(scope, "several operation collations");
  }
  return OperationCollation(collations.front(), scope);
}

std::string CollationKey(const std::string& sql, const std::string& specification) {
  if (specification.empty()) {
    return sql;
  }
  return "bq_collation_key(" + sql + ", " + QuoteLiteral(specification) + ")";
}

bool OnlyCollations(const googlesql::AnnotationMap* annotations) {
  if (annotations == nullptr) {
    return true;
  }
  auto others = annotations->Clone();
  others->UnsetAnnotationRecursively(kCollation);
  return others->Empty();
}

bool HasCollation(const googlesql::AnnotationMap* annotations) {
  bool found = false;
  if (annotations != nullptr) {
    EachCollation(*annotations, [&found](const std::string& specification) {
      found = found || !specification.empty();
    });
  }
  return found;
}

}  // namespace bigquery_emulator_duckdb::translator
