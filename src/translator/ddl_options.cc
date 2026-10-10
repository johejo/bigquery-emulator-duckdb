#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/statusor.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/type_parameters.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/table_metadata.h"
#include "src/translated_statement.h"
#include "src/translator/context.h"
#include "src/translator/ddl_internal.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

bool HasCollation(const googlesql::ResolvedColumnAnnotations* annotations) {
  if (annotations == nullptr) {
    return false;
  }
  if (annotations->collation_name() != nullptr) {
    return true;
  }
  return std::ranges::any_of(annotations->child_list(),
                             [](const auto& child) { return HasCollation(child.get()); });
}

// The type parameters of `column`, its nested fields' included, which its annotations keep
// apart.
std::optional<googlesql::TypeParameters> ColumnTypeParameters(
    const googlesql::ResolvedColumnDefinition& column, const Scope& scope) {
  if (column.annotations() == nullptr) {
    return googlesql::TypeParameters();
  }
  absl::StatusOr<googlesql::TypeParameters> parameters =
      column.annotations()->GetFullTypeParameters(column.type());
  if (!parameters.ok()) {
    return Unsupported(scope, parameters.status().message());
  }
  return *std::move(parameters);
}

// The labels option, an ARRAY<STRUCT<STRING, STRING>> of keys and values, into `into`.
bool LabelsOption(const googlesql::Value& value, std::map<std::string, std::string>& into) {
  into.clear();
  if (value.is_null()) {
    return true;
  }
  const googlesql::Type* type = value.type();
  if (!type->IsArray() || !type->AsArray()->element_type()->IsStruct()) {
    return false;
  }
  const googlesql::StructType* entry = type->AsArray()->element_type()->AsStruct();
  if (entry->num_fields() != 2 || !entry->field(0).type->IsString() ||
      !entry->field(1).type->IsString()) {
    return false;
  }
  for (int i = 0; i < value.num_elements(); ++i) {
    const googlesql::Value& element = value.element(i);
    if (element.is_null() || element.field(0).is_null() || element.field(1).is_null()) {
      return false;
    }
    into[element.field(0).string_value()] = element.field(1).string_value();
  }
  return true;
}

// The description, friendly name and labels the OPTIONS of a CREATE TABLE or CREATE VIEW set on
// `metadata`. The other options change how BigQuery stores, expires or reads the table, which the
bool ApplyAnnotations(const googlesql::Type* type,
                      const googlesql::ResolvedColumnAnnotations& annotations, FieldSchema& field,
                      const Scope& scope) {
  if (annotations.not_null() && field.mode == FieldMode::kNullable) {
    field.mode = FieldMode::kRequired;
  }
  for (const auto& option : annotations.option_list()) {
    const googlesql::Value* value = OptionLiteral(*option);
    if (ToLowerAscii(option->name()) != "description" || value == nullptr ||
        !StringOption(*value, field.description)) {
      Unsupported(scope, "column option " + option->name());
      return false;
    }
  }
  // An array's one child annotates its elements, which `field` describes too, and a struct's
  // children its fields.
  if (type->IsArray() && annotations.child_list_size() > 0) {
    return ApplyAnnotations(type->AsArray()->element_type(), *annotations.child_list(0), field,
                            scope);
  }
  if (type->IsStruct()) {
    for (int i = 0; i < annotations.child_list_size() && std::cmp_less(i, field.fields.size()) &&
                    i < type->AsStruct()->num_fields();
         ++i) {
      if (!ApplyAnnotations(type->AsStruct()->field(i).type, *annotations.child_list(i),
                            field.fields.at(i), scope)) {
        return false;
      }
    }
  }
  return true;
}

// Records the parameters of a parameterized type, STRING(10) or NUMERIC(10, 2), on `field`.
void ApplyTypeParameters(const googlesql::Type* type, const googlesql::TypeParameters& parameters,
                         FieldSchema& field) {
  if (parameters.IsStringTypeParameters()) {
    if (!parameters.string_type_parameters().is_max_length()) {
      field.max_length = parameters.string_type_parameters().max_length();
    }
    return;
  }
  if (parameters.IsNumericTypeParameters()) {
    const auto& numeric = parameters.numeric_type_parameters();
    if (!numeric.is_max_precision()) {
      field.precision = numeric.precision();
    }
    field.scale = numeric.scale();
    return;
  }
  if (type->IsArray() && parameters.num_children() > 0) {
    ApplyTypeParameters(type->AsArray()->element_type(), parameters.child(0), field);
  } else if (type->IsStruct()) {
    for (int i = 0; std::cmp_less(i, parameters.num_children()) &&
                    std::cmp_less(i, field.fields.size()) && i < type->AsStruct()->num_fields();
         ++i) {
      ApplyTypeParameters(type->AsStruct()->field(i).type, parameters.child(i), field.fields.at(i));
    }
  }
}

}  // namespace

std::optional<std::string> ColumnDefinitionType(const googlesql::ResolvedColumnDefinition& column,
                                                const Scope& scope) {
  if (HasInterval(column.type())) {
    return Unsupported(scope, "stored INTERVAL columns");
  }
  if (HasCollation(column.annotations())) {
    return Unsupported(scope, "column collation");
  }
  const auto parameters = ColumnTypeParameters(column, scope);
  if (!parameters) {
    return std::nullopt;
  }
  auto type = DuckDbType(column.type(), &*parameters);
  if (!type) {
    return Unsupported(scope, "column type " + column.type()->DebugString());
  }
  return type;
}

bool IfNotExists(const googlesql::ResolvedCreateStatement& create) {
  return create.create_mode() == googlesql::ResolvedCreateStatement::CREATE_IF_NOT_EXISTS;
}

// The literal an option of a CREATE statement assigns, or nullptr for any other value.
const googlesql::Value* OptionLiteral(const googlesql::ResolvedOption& option) {
  if (!option.qualifier().empty() ||
      option.assignment_op() != googlesql::ResolvedOption::DEFAULT_ASSIGN ||
      !option.value()->Is<googlesql::ResolvedLiteral>()) {
    return nullptr;
  }
  return &option.value()->GetAs<googlesql::ResolvedLiteral>()->value();
}

// A STRING option into `into`; NULL leaves it unset.
bool StringOption(const googlesql::Value& value, std::string& into) {
  if (value.is_null()) {
    into.clear();
    return true;
  }
  if (!value.type()->IsString()) {
    return false;
  }
  into = value.string_value();
  return true;
}

std::optional<TableMetadata> OptionsMetadata(const Options& options, std::string_view statement,
                                             const Scope& scope, TableMetadata metadata) {
  for (const auto& option : options) {
    const std::string name = ToLowerAscii(option->name());
    const googlesql::Value* value = OptionLiteral(*option);
    const bool applied =
        value != nullptr &&
        ((name == "description" && StringOption(*value, metadata.description)) ||
         (name == "friendly_name" && StringOption(*value, metadata.friendly_name)) ||
         (name == "labels" && LabelsOption(*value, metadata.labels)));
    if (!applied) {
      return Unsupported(scope, std::string(statement) + " option " + option->name());
    }
  }
  return metadata;
}

// The description, friendly name and labels the OPTIONS of a CREATE SCHEMA set. Every dataset is
// in the US, so a location elsewhere is unsupported, and so are the other options, which change
// how BigQuery treats the dataset's tables.
std::optional<DatasetMetadata> SchemaOptionsMetadata(const Options& options, const Scope& scope) {
  DatasetMetadata metadata;
  for (const auto& option : options) {
    const std::string name = ToLowerAscii(option->name());
    const googlesql::Value* value = OptionLiteral(*option);
    std::string location;
    const bool applied =
        value != nullptr &&
        ((name == "description" && StringOption(*value, metadata.description)) ||
         (name == "friendly_name" && StringOption(*value, metadata.friendly_name)) ||
         (name == "labels" && LabelsOption(*value, metadata.labels)) ||
         (name == "location" && StringOption(*value, location) &&
          (value->is_null() || ToUpperAscii(location) == "US")));
    if (!applied) {
      return Unsupported(scope, "CREATE SCHEMA option " + option->name());
    }
  }
  return metadata;
}

// The description, friendly name and labels SET OPTIONS of `statement` sets on `updates`. The
// other options change how BigQuery stores, expires or reads the table or dataset, which the
// emulator does not emulate, so they are unsupported, and so are += and -=.
bool SetOptions(const Options& options, std::string_view statement, const Scope& scope,
                OptionUpdates& updates) {
  for (const auto& option : options) {
    const std::string name = ToLowerAscii(option->name());
    const googlesql::Value* value = OptionLiteral(*option);
    const bool applied =
        value != nullptr &&
        ((name == "description" && StringOption(*value, updates.description.emplace())) ||
         (name == "friendly_name" && StringOption(*value, updates.friendly_name.emplace())) ||
         (name == "labels" && LabelsOption(*value, updates.labels.emplace())));
    if (!applied) {
      Unsupported(scope, std::string(statement) + " option " + option->name());
      return false;
    }
  }
  return true;
}

// Records NOT NULL as REQUIRED and the description in OPTIONS on `field`, and the same for the
// fields of a struct. Other column options are unsupported.
// The description ALTER COLUMN SET OPTIONS sets on `action`. The other column options change how
// BigQuery stores or reads the column, which the emulator does not emulate, so they are
// unsupported, and so are += and -=.
bool ColumnSetOptions(const Options& options, const Scope& scope, ColumnOptionsAction& action) {
  for (const auto& option : options) {
    const googlesql::Value* value = OptionLiteral(*option);
    if (ToLowerAscii(option->name()) != "description" || value == nullptr ||
        !StringOption(*value, action.description.emplace())) {
      Unsupported(scope, "column option " + option->name());
      return false;
    }
  }
  return true;
}

// The TableFieldSchema BigQuery reports for a column a DDL statement defines.
std::optional<FieldSchema> ColumnField(const googlesql::ResolvedColumnDefinition& column,
                                       const Scope& scope) {
  absl::StatusOr<FieldSchema> field = BigQueryFieldSchema(column.name(), column.type());
  if (!field.ok()) {
    return Unsupported(scope, field.status().message());
  }
  if (column.annotations() != nullptr) {
    if (!ApplyAnnotations(column.type(), *column.annotations(), *field, scope)) {
      return std::nullopt;
    }
    const auto parameters = ColumnTypeParameters(column, scope);
    if (!parameters) {
      return std::nullopt;
    }
    ApplyTypeParameters(column.type(), *parameters, *field);
  }
  if (column.default_value() != nullptr) {
    field->default_value_expression = column.default_value()->sql();
  }
  return *std::move(field);
}

}  // namespace bigquery_emulator_duckdb::translator
