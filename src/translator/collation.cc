#include "src/translator/collation.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
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

std::optional<Collation> OperationCollation(const googlesql::ResolvedCollation& collation,
                                            const Scope& scope) {
  Collation result{.specification = std::string(collation.CollationName()), .children = {}};
  result.children.reserve(collation.num_children());
  for (const auto& child : collation.child_list()) {
    const auto nested = OperationCollation(child, scope);
    if (!nested) {
      return std::nullopt;
    }
    result.children.push_back(*nested);
  }
  return result;
}

std::optional<Collation> OperationCollation(
    const std::vector<googlesql::ResolvedCollation>& collations, const Scope& scope) {
  if (collations.empty()) {
    return Collation{};
  }
  if (collations.size() > 1) {
    return Unsupported(scope, "several operation collations");
  }
  return OperationCollation(collations.front(), scope);
}

std::string CollationKey(const std::string& sql, const Collation& collation,
                         const googlesql::Type* type) {
  if (collation.empty()) {
    return sql;
  }
  if (collation.children.empty()) {
    return "bq_collation_key(" + sql + ", " + QuoteLiteral(collation.specification) + ")";
  }
  // Bind each container once, including volatile expressions. A NULL STRUCT must remain
  // NULL, rather than becoming a non-NULL row of NULL fields. Positional fields also handle
  // anonymous and duplicate STRUCT field names.
  if (type == nullptr) {
    throw std::invalid_argument("Nested collation requires a value type");
  }
  if (type->IsArray()) {
    return "list_transform(" + sql + ", lambda _ck: " +
           CollationKey("_ck", collation.children.at(0), type->AsArray()->element_type()) + ")";
  }
  std::vector<std::string> fields;
  fields.reserve(type->AsStruct()->num_fields());
  for (int i = 0; i < type->AsStruct()->num_fields(); ++i) {
    fields.push_back(CollationKey("struct_extract_at(_ck, " + std::to_string(i + 1) + ")",
                                  collation.children.at(i), type->AsStruct()->field(i).type));
  }
  return "list_transform([" + sql + "], lambda _ck: CASE WHEN _ck IS NULL THEN NULL ELSE row(" +
         Join(fields, ", ") + ") END)[1]";
}

bool OnlyCollations(const googlesql::AnnotationMap* annotations) {
  if (annotations == nullptr) {
    return true;
  }
  auto others = annotations->Clone();
  others->UnsetAnnotationRecursively(kCollation);
  return others->Empty();
}

Collation ColumnCollation(const googlesql::AnnotationMap* annotations) {
  if (annotations == nullptr) {
    return {};
  }
  Collation result;
  const googlesql::SimpleValue* collation = annotations->GetAnnotation(kCollation);
  if (collation != nullptr && collation->has_string_value()) {
    result.specification = collation->string_value();
  }
  if (const auto* fields = annotations->AsStructMap(); fields != nullptr) {
    for (int i = 0; i < fields->num_fields(); ++i) {
      result.children.push_back(ColumnCollation(fields->field(i)));
    }
  }
  if (std::ranges::all_of(result.children, [](const Collation& child) { return child.empty(); })) {
    result.children.clear();
  }
  return result;
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
