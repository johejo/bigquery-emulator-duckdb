#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "googlesql/public/cast.h"
#include "googlesql/public/function.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/type_parameters.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/catalog_table.h"
#include "src/column_default.h"
#include "src/ddl_option_types.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/references.h"
#include "src/statement_resources.h"
#include "src/table_metadata.h"
#include "src/table_path.h"
#include "src/table_source.h"
#include "src/translated_statement.h"
#include "src/translator/context.h"
#include "src/translator/ddl.h"
#include "src/translator/ddl_names.h"
#include "src/translator/ddl_options.h"
#include "src/translator/expression.h"
#include "src/translator/function.h"
#include "src/translator/scan.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {

namespace {

constexpr auto kDefaultExpressionFunctions = std::to_array<std::string_view>({
    "$make_array",
    "current_date",
    "current_datetime",
    "current_time",
    "current_timestamp",
    "generate_uuid",
    "rand",
    "session_user",
    "st_geogpoint",
});

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

bool ApplyColumnOption(DdlOptionId name, const googlesql::Value& value, FieldSchema& field) {
  switch (name) {
    case DdlOptionId::kDescription:
      return StringOption(value, field.description);
    case DdlOptionId::kRoundingMode:
      return RoundingModeOption(value, field.rounding_mode);
    default:
      return false;
  }
}

bool ApplyColumnOption(DdlOptionId name, const googlesql::Value& value,
                       ColumnOptionsAction& action) {
  switch (name) {
    case DdlOptionId::kDescription:
      return StringOption(value, action.description.emplace());
    case DdlOptionId::kRoundingMode:
      return RoundingModeOption(value, action.rounding_mode.emplace());
    default:
      return false;
  }
}

// Records NOT NULL as REQUIRED, the collation and the description and rounding mode in OPTIONS on
// `field`, and the same for the fields of a struct. Other column options are unsupported.
bool ApplyAnnotations(const googlesql::Type* type,
                      const googlesql::ResolvedColumnAnnotations& annotations, FieldSchema& field,
                      const Scope& scope) {
  if (annotations.not_null() && field.mode == FieldMode::kNullable) {
    field.mode = FieldMode::kRequired;
  }
  if (const googlesql::ResolvedExpr* collation = annotations.collation_name()) {
    if (!collation->Is<googlesql::ResolvedLiteral>() ||
        collation->GetAs<googlesql::ResolvedLiteral>()->value().is_null()) {
      Unsupported(scope, "column collation that is not a literal");
      return false;
    }
    std::string specification =
        collation->GetAs<googlesql::ResolvedLiteral>()->value().string_value();
    if (!specification.empty() && specification != "und:ci") {
      throw std::invalid_argument("Collation '" + specification + "' is unsupported.");
    }
    field.collation = std::move(specification);
  }
  for (const auto& option : annotations.option_list()) {
    const auto decoded = DecodeDdlOption(*option, DdlOptionContext::kColumn, scope);
    if (!decoded || !ApplyColumnOption(decoded->id, decoded->value, field)) {
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

std::optional<std::string> ColumnDefinitionType(const googlesql::ResolvedColumnDefinition& column,
                                                const Scope& scope) {
  const auto parameters = ColumnTypeParameters(column, scope);
  if (!parameters) {
    return std::nullopt;
  }
  auto type = DuckDbColumnType(column.type(), &*parameters);
  if (!type) {
    return Unsupported(scope, "column type " + column.type()->DebugString());
  }
  return type;
}

// The description and rounding mode ALTER COLUMN SET OPTIONS sets on `action`. The other column
// options change how BigQuery stores or reads the column, which the emulator does not emulate, so
// they are unsupported, and so are += and -=.
bool ColumnSetOptions(const Options& options, const Scope& scope, ColumnOptionsAction& action) {
  for (const auto& option : options) {
    const auto decoded = DecodeDdlOption(*option, DdlOptionContext::kColumn, scope);
    if (!decoded || !ApplyColumnOption(decoded->id, decoded->value, action)) {
      Unsupported(scope, "column option " + option->name());
      return false;
    }
  }
  return true;
}

// Array defaults, including arrays within structs, cannot contain NULL elements.
bool ValidDefaultValue(const googlesql::Value& value, bool array_element) {
  if (value.is_null()) {
    return !array_element && !value.type()->IsArray();
  }
  if (value.type()->IsArray()) {
    for (int i = 0; i < value.num_elements(); ++i) {
      if (!ValidDefaultValue(value.element(i), true)) {
        return false;
      }
    }
  } else if (value.type()->IsStruct()) {
    for (int i = 0; i < value.num_fields(); ++i) {
      if (!ValidDefaultValue(value.field(i), false)) {
        return false;
      }
    }
  }
  return true;
}

// A cast without formatting, an extended conversion or SAFE error handling.
bool IsPlainCast(const googlesql::ResolvedCast& cast) {
  return cast.format() == nullptr && cast.time_zone() == nullptr &&
         cast.extended_cast() == nullptr && !cast.return_null_on_error();
}

// GoogleSQL resolves more expressions than BigQuery allows for defaults. Restrict SET
// DEFAULT to literals, constructors and the documented functions, including non-NULL arrays.
bool SupportedDefaultExpression(const googlesql::ResolvedExpr& expression, const Scope& scope,
                                bool array_element = false) {
  if (expression.Is<googlesql::ResolvedLiteral>()) {
    const auto& value = expression.GetAs<googlesql::ResolvedLiteral>()->value();
    if (!ValidDefaultValue(value, array_element)) {
      Unsupported(scope, "NULL array default or element");
      return false;
    }
    return true;
  }
  if (expression.Is<googlesql::ResolvedCast>()) {
    const auto& cast = *expression.GetAs<googlesql::ResolvedCast>();
    if (IsPlainCast(cast)) {
      return SupportedDefaultExpression(*cast.expr(), scope,
                                        array_element || cast.type()->IsArray());
    }
  } else if (expression.Is<googlesql::ResolvedMakeStruct>()) {
    const auto& record = *expression.GetAs<googlesql::ResolvedMakeStruct>();
    return std::ranges::all_of(record.field_list(), [&](const auto& field) {
      return SupportedDefaultExpression(*field, scope);
    });
  } else if (expression.Is<googlesql::ResolvedFunctionCall>()) {
    const auto& call = *expression.GetAs<googlesql::ResolvedFunctionCall>();
    const std::string name = ToLowerAscii(call.function()->Name());
    if (call.function()->IsGoogleSQLBuiltin() &&
        std::ranges::find(kDefaultExpressionFunctions, name) != kDefaultExpressionFunctions.end()) {
      return std::ranges::all_of(call.argument_list(), [&](const auto& argument) {
        return SupportedDefaultExpression(*argument, scope, name == "$make_array");
      });
    }
  }
  Unsupported(scope, "column default expression");
  return false;
}

// Translate the expression, leaving parameterized column constraints to storage.
std::optional<std::string> ColumnDefaultSql(const googlesql::ResolvedColumnDefaultValue& value,
                                            const Scope& scope) {
  const googlesql::ResolvedExpr* expression = value.expression();
  if (expression->Is<googlesql::ResolvedCast>()) {
    const auto* cast = expression->GetAs<googlesql::ResolvedCast>();
    if (IsPlainCast(*cast)) {
      expression = cast->expr();
    }
  }
  return Expression(*expression, scope, {});
}

// Throws BigQuery's error for a field of `field` whose name differs only in case from another's.
// The error names it by its path below `path`: the column for CREATE TABLE, but nothing for ADD
// COLUMN.
void CheckFieldNames(const FieldSchema& field, const std::string& path) {
  if (const std::optional<std::string> duplicate = DuplicateFieldPath(field.fields, path)) {
    throw std::invalid_argument("Invalid value: Field " + *duplicate + " already exists in schema");
  }
}

// A literal default may have an implicit cast carrying its column's type parameters.
std::optional<googlesql::Value> DefaultLiteral(const googlesql::ResolvedExpr& expression) {
  if (expression.Is<googlesql::ResolvedLiteral>()) {
    return expression.GetAs<googlesql::ResolvedLiteral>()->value();
  }
  if (expression.Is<googlesql::ResolvedCast>()) {
    const auto& cast = *expression.GetAs<googlesql::ResolvedCast>();
    if (IsPlainCast(cast)) {
      if (const auto literal = DefaultLiteral(*cast.expr())) {
        const auto value = googlesql::CastValue(*literal, absl::UTCTimeZone(),
                                                googlesql::LanguageOptions(), cast.type());
        if (!value.ok()) {
          throw std::invalid_argument(std::string(value.status().message()));
        }
        return *value;
      }
    }
  }
  return std::nullopt;
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
    const auto& expression = *column.default_value()->expression();
    ValidateColumnDefault(*field, DefaultLiteral(expression));
    field->default_value_expression = column.default_value()->sql();
  }
  return *std::move(field);
}

// The temporary table CREATE TEMP TABLE `path` creates. The analyzer rejects one outside a
// multi-statement query, and one with a qualified name.
std::optional<std::string> TemporaryTargetTable(const std::vector<std::string>& path,
                                                const Scope& scope) {
  const TemporaryTables* temporary = scope.context.defaults.temporary;
  const std::optional<std::string> name = TemporaryTableName(path);
  if (temporary == nullptr || !name) {
    return Unsupported(scope, "temporary table " + Join(path, "."));
  }
  scope.context.statement.ddl_target_table = TableReference{
      .project_id = temporary->project,
      .dataset_id = temporary->dataset,
      .table_id = *name,
  };
  return QualifiedName(*scope.context.statement.ddl_target_table);
}

// The column `expression` reads, or "" when it is not a column.
std::string ColumnName(const googlesql::ResolvedExpr& expression) {
  return expression.Is<googlesql::ResolvedColumnRef>()
             ? expression.GetAs<googlesql::ResolvedColumnRef>()->column().name()
             : "";
}

// The INT64 literals of GENERATE_ARRAY(start, end[, interval]) as a range.
std::optional<RangePartitioning> GenerateArrayRange(const googlesql::ResolvedExpr& expression,
                                                    const std::string& field) {
  if (!expression.Is<googlesql::ResolvedFunctionCall>()) {
    return std::nullopt;
  }
  const auto& call = *expression.GetAs<googlesql::ResolvedFunctionCall>();
  std::vector<int64_t> values;
  for (const auto& argument : call.argument_list()) {
    if (!argument->Is<googlesql::ResolvedLiteral>()) {
      return std::nullopt;
    }
    const googlesql::Value& value = argument->GetAs<googlesql::ResolvedLiteral>()->value();
    if (value.is_null() || !value.type()->IsInt64()) {
      return std::nullopt;
    }
    values.push_back(value.int64_value());
  }
  if (call.function()->Name() != "generate_array" || values.size() < 2) {
    return std::nullopt;
  }
  return RangePartitioning{
      .field = field,
      .start = values.at(0),
      .end = values.at(1),
      .interval = values.size() > 2 ? values.at(2) : 1,
  };
}

// Records the partitioning that a PARTITION BY expression gives on `metadata`: a DATE column,
// DATE of a TIMESTAMP or DATETIME column, TIMESTAMP_TRUNC, DATETIME_TRUNC or DATE_TRUNC of one,
// or RANGE_BUCKET of an INT64 column over GENERATE_ARRAY. Partitioning by ingestion time adds
// pseudo-columns, which the emulator does not have.
bool Partitioning(const googlesql::ResolvedExpr& expression, TableMetadata& metadata) {
  if (const std::string column = ColumnName(expression); !column.empty()) {
    metadata.time_partitioning = TimePartitioning{.type = TimePartitionUnit::kDay, .field = column};
    return expression.type()->IsDate();
  }
  if (!expression.Is<googlesql::ResolvedFunctionCall>()) {
    return false;
  }
  const auto& call = *expression.GetAs<googlesql::ResolvedFunctionCall>();
  const std::string function = call.function()->Name();
  const auto& arguments = call.argument_list();
  const std::string column = arguments.empty() ? "" : ColumnName(*arguments.at(0));
  if (column.empty()) {
    return false;
  }
  const googlesql::Type* type = arguments.at(0)->type();
  if (function == "date" && arguments.size() == 1) {
    metadata.time_partitioning = TimePartitioning{.type = TimePartitionUnit::kDay, .field = column};
    return type->IsTimestamp() || type->IsDatetime();
  }
  if (function == "range_bucket" && arguments.size() == 2) {
    metadata.range_partitioning = GenerateArrayRange(*arguments.at(1), column);
    return metadata.range_partitioning.has_value();
  }
  const bool truncates = (function == "timestamp_trunc" && type->IsTimestamp()) ||
                         (function == "datetime_trunc" && type->IsDatetime()) ||
                         (function == "date_trunc" && type->IsDate());
  if (!truncates || arguments.size() != 2 || !arguments.at(1)->Is<googlesql::ResolvedLiteral>()) {
    return false;
  }
  const std::string part =
      arguments.at(1)->GetAs<googlesql::ResolvedLiteral>()->value().EnumDisplayName();
  const auto unit = ParseTimePartitionUnit(part);
  if (!unit) {
    return false;
  }
  metadata.time_partitioning = TimePartitioning{.type = *unit, .field = column};
  return *unit == TimePartitionUnit::kMonth || *unit == TimePartitionUnit::kYear ||
         (!type->IsDate() &&
          (*unit == TimePartitionUnit::kDay || *unit == TimePartitionUnit::kHour));
}

// Records the PARTITION BY and CLUSTER BY of a CREATE TABLE [AS SELECT] on `metadata`.
template <typename CreateTable>
bool PartitioningAndClustering(const CreateTable& create, TableMetadata& metadata,
                               const Scope& scope) {
  for (const auto& expression : create.partition_by_list()) {
    // Validate and record partition metadata in order, stopping on the first error.
    // cppcheck-suppress useStlAlgorithm
    if (create.partition_by_list_size() != 1 || !Partitioning(*expression, metadata)) {
      Unsupported(scope, "this PARTITION BY expression");
      return false;
    }
  }
  for (const auto& expression : create.cluster_by_list()) {
    const std::string column = ColumnName(*expression);
    if (column.empty()) {
      Unsupported(scope, "this CLUSTER BY expression");
      return false;
    }
    metadata.clustering.push_back(column);
  }
  return true;
}

// Whether the emulator takes `key`; BigQuery takes only an unnamed NOT ENFORCED primary key,
// without options.
bool SupportedPrimaryKey(const googlesql::ResolvedPrimaryKey& key, const Scope& scope) {
  if (!key.unenforced()) {
    throw std::invalid_argument("Enforcement of primary keys is not supported");
  }
  if (!key.constraint_name().empty() || key.option_list_size() > 0) {
    Unsupported(scope, "named primary keys or primary key options");
    return false;
  }
  return true;
}

// Records the primary key of `create` on `metadata`. Foreign keys are dropped.
bool PrimaryKey(const googlesql::ResolvedCreateTableStmtBase& create, TableMetadata& metadata,
                const Scope& scope) {
  const googlesql::ResolvedPrimaryKey* key = create.primary_key();
  if (key == nullptr) {
    return true;
  }
  if (!SupportedPrimaryKey(*key, scope)) {
    return false;
  }
  for (const int offset : key->column_offset_list()) {
    metadata.primary_key.push_back(create.column_definition_list(offset)->name());
  }
  return true;
}

std::optional<std::string> CreateTableHead(const googlesql::ResolvedCreateTableStmtBase& create,
                                           const Scope& scope) {
  if (create.create_scope() == googlesql::ResolvedCreateStatement::CREATE_TEMP &&
      !create.option_list().empty()) {
    throw std::invalid_argument("OPTIONS on temporary tables are not supported");
  }
  if (create.is_value_table() || !create.pseudo_column_list().empty() ||
      create.connection_list() != nullptr || !create.check_constraint_list().empty()) {
    return Unsupported(scope, "CREATE TABLE option");
  }
  const auto path = create.create_scope() == googlesql::ResolvedCreateStatement::CREATE_TEMP
                        ? TemporaryTargetTable(create.name_path(), scope)
                        : TargetTable(create.name_path(), scope, true);
  if (!path) {
    return std::nullopt;
  }
  switch (create.create_mode()) {
    case googlesql::ResolvedCreateStatement::CREATE_OR_REPLACE:
      scope.context.statement.replaces = true;
      return "CREATE OR REPLACE TABLE " + *path;
    case googlesql::ResolvedCreateStatement::CREATE_IF_NOT_EXISTS:
      return "CREATE TABLE IF NOT EXISTS " + *path;
    default:
      return "CREATE TABLE " + *path;
  }
}

// The schema and metadata of `table`, the source of `statement`, which is a CREATE TABLE LIKE,
// COPY or CLONE or a CREATE SNAPSHOT TABLE, of a temporary table when `temporary`. A clone's
// cloneDefinition is its own, which no copy inherits. BigQuery rejects a view, a table snapshot
// as the source of LIKE or COPY, and a temporary table of a table that is not temporary, with
// these messages.
// The query of the rows that CREATE TABLE COPY, CLONE or CREATE SNAPSHOT TABLE copies from `scan`,
// a scan of a table SourceDescription accepted, or none for the rows it has now that need no
// alternative read query. Sets `time` to the time FOR SYSTEM_TIME AS OF reads it at, as
// tables.get reports it.
std::optional<std::optional<std::string>> SourceRows(const googlesql::ResolvedTableScan& scan,
                                                     const Scope& scope, std::string& time) {
  if (scan.for_system_time_expr() == nullptr) {
    const auto& table = *dynamic_cast<const BigQueryTable*>(scan.table());
    return StatementResources::ReadQuery(table.reference(), table.ReadQuery(),
                                         scope.context.resources);
  }
  int64_t micros = 0;
  std::optional<std::string> rows = TimeTravelRows(scan, scope, micros);
  if (!rows) {
    return std::nullopt;
  }
  time =
      absl::FormatTime("%Y-%m-%dT%H:%M:%E3SZ", absl::FromUnixMicros(micros), absl::UTCTimeZone());
  return rows;
}

std::optional<TableDescription> SourceDescription(const googlesql::Table* table,
                                                  std::string_view statement, bool temporary,
                                                  const Scope& scope) {
  const auto* source = dynamic_cast<const BigQueryTable*>(table);
  if (source == nullptr) {
    return Unsupported(scope, std::string(statement) + " of this table");
  }
  const TemporaryTables* temporaries = scope.context.defaults.temporary;
  if (temporary &&
      (temporaries == nullptr || source->reference().project_id != temporaries->project ||
       source->reference().dataset_id != temporaries->dataset)) {
    throw std::invalid_argument(std::format(
        "CREATE TEMPORARY TABLE ... {} is only allowed if the source table is a temporary table.",
        statement.substr(std::string_view("CREATE TABLE ").size())));
  }
  std::optional<TableDescription> description = source->Describe();
  // CLONE restores a table snapshot and CREATE SNAPSHOT TABLE copies one, which LIKE and COPY
  // reject as they reject a view.
  const bool takes_snapshots =
      statement == "CREATE TABLE CLONE" || statement == "CREATE SNAPSHOT TABLE";
  const char* rejected = !description ? "VIEW"
                         : description->metadata.snapshot.has_value() && !takes_snapshots
                             ? "SNAPSHOT"
                             : nullptr;
  if (rejected != nullptr) {
    const TableReference reference = source->reference();
    if (statement == "CREATE TABLE LIKE") {
      throw std::invalid_argument(
          std::format("Invalid value: Cannot CREATE TABLE LIKE `{}.{}`, "
                      "which is {}.",
                      reference.dataset_id, reference.table_id, rejected));
    }
    throw std::invalid_argument(
        std::format("Invalid value: Cannot {} {}:{}.{} which has type {}. Allowed types: [{}]",
                    statement == "CREATE SNAPSHOT TABLE" ? "snapshot" : "clone",
                    reference.project_id, reference.dataset_id, reference.table_id, rejected,
                    takes_snapshots ? "TABLE, SNAPSHOT" : "TABLE"));
  }
  if (description) {
    description->metadata.clone.reset();
  }
  return description;
}

// The columns of CREATE TABLE for `schema`.
std::optional<std::string> ColumnsSql(const std::vector<FieldSchema>& schema, const Scope& scope) {
  std::vector<std::string> columns;
  for (const FieldSchema& field : schema) {
    const absl::StatusOr<std::string> type = DuckDbColumnType(field);
    if (!type.ok()) {
      return Unsupported(scope, type.status().message());
    }
    columns.push_back(QuoteIdentifier(field.name) + " " + *type +
                      (field.mode == FieldMode::kRequired ? " NOT NULL" : ""));
  }
  return Join(columns, ", ");
}

// CREATE TABLE LIKE copies the schema, partitioning, clustering, description, friendly name,
// default rounding mode and labels of the source table, which PARTITION BY, CLUSTER BY and OPTIONS
// replace, as ALTER TABLE SET OPTIONS does.
template <typename CreateTable>
std::optional<TableDescription> LikeDescription(const CreateTable& create, const Scope& scope) {
  std::optional<TableDescription> description = SourceDescription(
      create.like_table(), "CREATE TABLE LIKE",
      create.create_scope() == googlesql::ResolvedCreateStatement::CREATE_TEMP, scope);
  if (!description) {
    return std::nullopt;
  }
  TableMetadata& inherited = description->metadata;
  if (create.partition_by_list_size() > 0) {
    inherited.time_partitioning.reset();
    inherited.range_partitioning.reset();
  }
  if (create.cluster_by_list_size() > 0) {
    inherited.clustering.clear();
  }
  auto metadata = OptionsMetadata(create.option_list(), DdlOptionContext::kCreateTable, scope,
                                  std::move(inherited));
  if (!metadata || !DefaultCollation(create.collation_name(), scope, metadata->default_collation) ||
      !PartitioningAndClustering(create, *metadata, scope)) {
    return std::nullopt;
  }
  description->metadata = *std::move(metadata);
  return description;
}

std::optional<std::string> CreateTableLike(const googlesql::ResolvedCreateTableStmt& create,
                                           const Scope& scope) {
  auto description = LikeDescription(create, scope);
  const auto head = CreateTableHead(create, scope);
  const std::optional<TableReference> target = scope.context.statement.ddl_target_table;
  if (!description || !head || !target) {
    return std::nullopt;
  }
  const auto columns = ColumnsSql(description->schema, scope);
  if (!columns) {
    return std::nullopt;
  }
  scope.context.statement.table = TableDefinition{
      .table = *target,
      .schema = std::move(description->schema),
      .metadata = std::move(description->metadata),
      .if_not_exists =
          (create.create_mode() == googlesql::ResolvedCreateStatement::CREATE_IF_NOT_EXISTS),
      .temporary = create.create_scope() == googlesql::ResolvedCreateStatement::CREATE_TEMP,
      .copies_table = true,
      .keeps_rounding_modes = true,
  };
  return *head + " (" + *columns + ")";
}

// CREATE TABLE COPY and CLONE copy the rows of the source table besides what CREATE TABLE LIKE
// copies, which OPTIONS replace; the analyzer rejects PARTITION BY and CLUSTER BY with them. A
// clone also records the table it was cloned from and when, which tables.get reports as its
// cloneDefinition. A clone of a table snapshot restores it as a standard table, which records no
// clone and, as BigQuery documents, takes its dataset's default expiration rather than the
// snapshot's. FOR SYSTEM_TIME AS OF copies the rows the table had then, which a clone records as
// its cloneTime.
std::optional<std::string> CreateTableCopy(const googlesql::ResolvedCreateTableStmt& create,
                                           const Scope& scope) {
  const bool clone = create.clone_from() != nullptr;
  const std::string kind = clone ? "CLONE" : "COPY";
  const std::string statement = "CREATE TABLE " + kind;
  const googlesql::ResolvedScan& from = clone ? *create.clone_from() : *create.copy_from();
  if (!from.Is<googlesql::ResolvedTableScan>()) {
    return Unsupported(scope, statement + " with WHERE");
  }
  const auto& scan = *from.GetAs<googlesql::ResolvedTableScan>();
  const bool temporary = create.create_scope() == googlesql::ResolvedCreateStatement::CREATE_TEMP;
  std::optional<TableDescription> description =
      SourceDescription(scan.table(), statement, temporary, scope);
  if (!description) {
    return std::nullopt;
  }
  std::string time;
  auto rows = SourceRows(scan, scope, time);
  if (!rows) {
    return std::nullopt;
  }
  const bool replace =
      create.create_mode() == googlesql::ResolvedCreateStatement::CREATE_OR_REPLACE;
  const TableReference source = dynamic_cast<const BigQueryTable*>(scan.table())->reference();
  const auto head = CreateTableHead(create, scope);
  const std::optional<TableReference> target = scope.context.statement.ddl_target_table;
  const bool restore = description->metadata.snapshot.has_value();
  if (restore) {
    description->metadata.restored_from_snapshot = true;
    description->metadata.snapshot.reset();
    description->metadata.expiration_time.reset();
  }
  if (clone && !restore) {
    description->metadata.clone = BaseTableDefinition{.base_table = source, .time = time};
  }
  auto metadata = OptionsMetadata(create.option_list(), DdlOptionContext::kCreateTable, scope,
                                  std::move(description->metadata));
  const auto columns = ColumnsSql(description->schema, scope);
  if (!head || !target || !metadata || !columns ||
      !DefaultCollation(create.collation_name(), scope, metadata->default_collation)) {
    return std::nullopt;
  }
  // The new table would replace the source before its rows are copied.
  if (replace && ToLowerAscii(QualifiedName(*target)) == ToLowerAscii(QualifiedName(source))) {
    return Unsupported(scope, "CREATE OR REPLACE TABLE " + kind + " of itself");
  }
  scope.context.statement.table = TableDefinition{
      .table = *target,
      .schema = std::move(description->schema),
      .metadata = *std::move(metadata),
      .if_not_exists =
          (create.create_mode() == googlesql::ResolvedCreateStatement::CREATE_IF_NOT_EXISTS),
      .rows_from = source,
      .rows_query = *std::move(rows),
      .temporary = temporary,
      .copies_table = true,
      .keeps_rounding_modes = true,
  };
  return *head + " (" + *columns + ")";
}

}  // namespace

// ALTER TABLE ADD COLUMN, DROP COLUMN, RENAME TO, ALTER COLUMN SET OPTIONS, SET/DROP DEFAULT,
// ADD and DROP PRIMARY KEY and SET OPTIONS, applied to the table as it is when the statement runs.
std::optional<std::string> AlterTable(const googlesql::ResolvedAlterTableStmt& alter,
                                      const Scope& scope) {
  const auto path = TargetTable(alter.name_path(), scope);
  const std::optional<TableReference> target = scope.context.statement.ddl_target_table;
  if (!path || !target) {
    return std::nullopt;
  }
  TableAlteration alteration{.table = *target, .if_exists = alter.is_if_exists()};
  // BigQuery rejects an enforced primary key before anything else about the statement.
  if (std::ranges::any_of(alter.alter_action_list(), [&](const auto& action) {
        if (!action->template Is<googlesql::ResolvedAddConstraintAction>()) {
          return false;
        }
        const auto* constraint =
            action->template GetAs<googlesql::ResolvedAddConstraintAction>()->constraint();
        return constraint->template Is<googlesql::ResolvedPrimaryKey>() &&
               !SupportedPrimaryKey(*constraint->template GetAs<googlesql::ResolvedPrimaryKey>(),
                                    scope);
      })) {
    return std::nullopt;
  }
  for (const auto& action : alter.alter_action_list()) {
    // BigQuery documents a list of ADD COLUMN or of DROP COLUMN actions, but not a mix of kinds.
    if (action->node_kind() != alter.alter_action_list(0)->node_kind()) {
      return Unsupported(scope, "ALTER TABLE with different kinds of actions");
    }
    if (action->Is<googlesql::ResolvedAddColumnAction>()) {
      const auto& add = *action->GetAs<googlesql::ResolvedAddColumnAction>();
      const auto& column = *add.column_definition();
      if (column.is_hidden() || column.generated_column_info() != nullptr) {
        return Unsupported(scope, "ADD COLUMN with generated columns");
      }
      if (column.default_value() != nullptr) {
        throw std::invalid_argument(
            "Cannot add a column with a default value to an existing table");
      }
      if (column.annotations() != nullptr && column.annotations()->not_null()) {
        throw std::invalid_argument("Cannot add a REQUIRED column to an existing table schema");
      }
      auto field = ColumnField(column, scope);
      if (!field) {
        return std::nullopt;
      }
      CheckFieldNames(*field, "");
      if (!ColumnDefinitionType(column, scope)) {
        return std::nullopt;
      }
      alteration.actions.emplace_back(
          AddColumnAction{.field = *std::move(field), .if_not_exists = add.is_if_not_exists()});
    } else if (action->Is<googlesql::ResolvedDropColumnAction>()) {
      const auto& drop = *action->GetAs<googlesql::ResolvedDropColumnAction>();
      alteration.actions.emplace_back(
          DropColumnAction{.name = drop.name(), .if_exists = drop.is_if_exists()});
    } else if (action->Is<googlesql::ResolvedRenameToAction>()) {
      // BigQuery takes only the new name of the table, which stays in its dataset.
      const auto& new_path = action->GetAs<googlesql::ResolvedRenameToAction>()->new_path();
      if (new_path.size() != 1 || new_path.at(0).find('.') != std::string::npos) {
        return Unsupported(scope, "RENAME TO a table path");
      }
      alteration.actions.emplace_back(RenameTableAction{.table_id = new_path.at(0)});
    } else if (action->Is<googlesql::ResolvedAlterColumnOptionsAction>()) {
      const auto& alter_column = *action->GetAs<googlesql::ResolvedAlterColumnOptionsAction>();
      ColumnOptionsAction column_options{
          .name = alter_column.column(),
          .if_exists = alter_column.is_if_exists(),
      };
      if (!ColumnSetOptions(alter_column.option_list(), scope, column_options)) {
        return std::nullopt;
      }
      alteration.actions.emplace_back(std::move(column_options));
    } else if (action->Is<googlesql::ResolvedAlterColumnSetDefaultAction>()) {
      const auto& set = *action->GetAs<googlesql::ResolvedAlterColumnSetDefaultAction>();
      if (!SupportedDefaultExpression(*set.default_value()->expression(), scope)) {
        return std::nullopt;
      }
      const auto sql = ColumnDefaultSql(*set.default_value(), scope);
      if (!sql) {
        return std::nullopt;
      }
      alteration.actions.emplace_back(ColumnDefaultAction{
          .name = set.column(),
          .expression = set.default_value()->sql(),
          .sql = *sql,
          .literal = set.default_value()->expression()->Is<googlesql::ResolvedLiteral>()
                         ? std::make_shared<const googlesql::Value>(
                               set.default_value()
                                   ->expression()
                                   ->GetAs<googlesql::ResolvedLiteral>()
                                   ->value())
                         : nullptr,
          .if_exists = set.is_if_exists(),
      });
    } else if (action->Is<googlesql::ResolvedAlterColumnDropDefaultAction>()) {
      const auto& drop = *action->GetAs<googlesql::ResolvedAlterColumnDropDefaultAction>();
      alteration.actions.emplace_back(ColumnDefaultAction{
          .name = drop.column(),
          .drop = true,
          .if_exists = drop.is_if_exists(),
      });
    } else if (action->Is<googlesql::ResolvedAddConstraintAction>() &&
               action->GetAs<googlesql::ResolvedAddConstraintAction>()
                   ->constraint()
                   ->Is<googlesql::ResolvedPrimaryKey>()) {
      const auto& key = *action->GetAs<googlesql::ResolvedAddConstraintAction>()
                             ->constraint()
                             ->GetAs<googlesql::ResolvedPrimaryKey>();
      alteration.actions.emplace_back(PrimaryKeyAction{.columns = key.column_name_list()});
    } else if (action->Is<googlesql::ResolvedDropPrimaryKeyAction>()) {
      alteration.actions.emplace_back(PrimaryKeyAction{
          .drop = true,
          .if_exists = action->GetAs<googlesql::ResolvedDropPrimaryKeyAction>()->is_if_exists(),
      });
    } else if (action->Is<googlesql::ResolvedSetCollateClause>()) {
      OptionUpdates updates;
      if (!DefaultCollation(action->GetAs<googlesql::ResolvedSetCollateClause>()->collation_name(),
                            scope, updates.default_collation)) {
        return std::nullopt;
      }
      alteration.actions.emplace_back(std::move(updates));
    } else if (action->Is<googlesql::ResolvedSetOptionsAction>()) {
      OptionUpdates updates;
      if (!SetOptions(action->GetAs<googlesql::ResolvedSetOptionsAction>()->option_list(),
                      DdlOptionContext::kAlterTable, scope, updates)) {
        return std::nullopt;
      }
      alteration.actions.emplace_back(std::move(updates));
    } else {
      return Unsupported(scope, "ALTER TABLE action " + action->node_kind_string());
    }
  }
  scope.context.statement.altered_table = std::move(alteration);
  return "";
}

// The analyzer gives an ALTER TABLE with only SET OPTIONS actions as this statement.
std::optional<std::string> AlterTableSetOptions(
    const googlesql::ResolvedAlterTableSetOptionsStmt& alter, const Scope& scope) {
  const auto path = TargetTable(alter.name_path(), scope);
  const std::optional<TableReference> target = scope.context.statement.ddl_target_table;
  OptionUpdates updates;
  if (!path || !target ||
      !SetOptions(alter.option_list(), DdlOptionContext::kAlterTable, scope, updates)) {
    return std::nullopt;
  }
  scope.context.statement.altered_table = TableAlteration{
      .table = *target,
      .actions = {std::move(updates)},
      .if_exists = alter.is_if_exists(),
  };
  return "";
}

std::optional<std::string> CreateTable(const googlesql::ResolvedCreateTableStmt& create,
                                       const Scope& scope) {
  if (create.clone_from() != nullptr || create.copy_from() != nullptr) {
    return CreateTableCopy(create, scope);
  }
  if (create.like_table() != nullptr) {
    return CreateTableLike(create, scope);
  }
  const auto head = CreateTableHead(create, scope);
  const std::optional<TableReference> target = scope.context.statement.ddl_target_table;
  auto metadata = OptionsMetadata(create.option_list(), DdlOptionContext::kCreateTable, scope);
  if (!head || !target || !metadata ||
      !DefaultCollation(create.collation_name(), scope, metadata->default_collation) ||
      !PartitioningAndClustering(create, *metadata, scope) ||
      !PrimaryKey(create, *metadata, scope)) {
    return std::nullopt;
  }
  TableDefinition table{
      .table = *target,
      .metadata = *std::move(metadata),
      .if_not_exists =
          (create.create_mode() == googlesql::ResolvedCreateStatement::CREATE_IF_NOT_EXISTS),
      .temporary = create.create_scope() == googlesql::ResolvedCreateStatement::CREATE_TEMP,
  };
  std::vector<std::string> columns;
  for (const auto& column : create.column_definition_list()) {
    if (column->is_hidden() || column->generated_column_info() != nullptr) {
      return Unsupported(scope, "generated columns");
    }
    auto field = ColumnField(*column, scope);
    if (!field) {
      return std::nullopt;
    }
    CheckFieldNames(*field, field->name);
    const auto type = ColumnDefinitionType(*column, scope);
    if (!type) {
      return std::nullopt;
    }
    table.schema.push_back(*std::move(field));
    std::string sql = QuoteIdentifier(column->name()) + " " + *type;
    if (column->annotations() != nullptr && column->annotations()->not_null()) {
      sql += " NOT NULL";
    }
    if (column->default_value() != nullptr) {
      const auto value = ColumnDefaultSql(*column->default_value(), scope);
      if (!value) {
        return std::nullopt;
      }
      sql += " DEFAULT " + *value;
    }
    columns.push_back(sql);
  }
  if (columns.empty()) {
    return Unsupported(scope, "CREATE TABLE without columns");
  }
  scope.context.statement.table = std::move(table);
  return *head + " (" + Join(columns, ", ") + ")";
}

// CREATE SNAPSHOT TABLE copies a table as CLONE does, into a table snapshot, which records the
// table it was made of and when as its snapshotDefinition and takes only the options BigQuery
// documents for it, but not the base table's expiration, as BigQuery answers. A snapshot of a
// snapshot names that snapshot as its base table. BigQuery takes neither OR REPLACE nor TEMP. FOR
// SYSTEM_TIME AS OF copies the rows the table had then, which the snapshot records as its
// snapshotTime.
std::optional<std::string> CreateSnapshotTable(
    const googlesql::ResolvedCreateSnapshotTableStmt& create, const Scope& scope) {
  const std::string statement = "CREATE SNAPSHOT TABLE";
  if (create.create_mode() == googlesql::ResolvedCreateStatement::CREATE_OR_REPLACE ||
      create.create_scope() != googlesql::ResolvedCreateStatement::CREATE_DEFAULT_SCOPE) {
    throw std::invalid_argument("CREATE SNAPSHOT TABLE takes neither OR REPLACE nor TEMP");
  }
  if (!create.clone_from()->Is<googlesql::ResolvedTableScan>()) {
    return Unsupported(scope, statement + " with WHERE");
  }
  const auto& scan = *create.clone_from()->GetAs<googlesql::ResolvedTableScan>();
  std::optional<TableDescription> description =
      SourceDescription(scan.table(), statement, /*temporary=*/false, scope);
  if (!description) {
    return std::nullopt;
  }
  std::string time;
  auto rows = SourceRows(scan, scope, time);
  if (!rows) {
    return std::nullopt;
  }
  const auto* stored = dynamic_cast<const BigQueryTable*>(scan.table());
  const TableReference source = stored->reference();
  // BigQuery names a temporary table by the job's project and a dataset of the script's, as the
  // emulator names the database of its temporary tables.
  TableReference base = source;
  if (const TemporaryTables* temporaries = scope.context.defaults.temporary;
      temporaries != nullptr && source.project_id == temporaries->project &&
      source.dataset_id == temporaries->dataset) {
    base = {
        .project_id = scope.context.defaults.project,
        .dataset_id = temporaries->project,
        .table_id = source.table_id,
    };
  }
  const auto path = TargetTable(create.name_path(), scope, true);
  const std::optional<TableReference> target = scope.context.statement.ddl_target_table;
  description->metadata.snapshot = BaseTableDefinition{.base_table = base, .time = time};
  description->metadata.expiration_time.reset();
  auto metadata = OptionsMetadata(create.option_list(), DdlOptionContext::kCreateSnapshotTable,
                                  scope, std::move(description->metadata));
  const auto columns = ColumnsSql(description->schema, scope);
  if (!path || !target || !metadata || !columns) {
    return std::nullopt;
  }
  const bool if_not_exists =
      create.create_mode() == googlesql::ResolvedCreateStatement::CREATE_IF_NOT_EXISTS;
  scope.context.statement.table = TableDefinition{
      .table = *target,
      .schema = std::move(description->schema),
      .metadata = *std::move(metadata),
      .if_not_exists = if_not_exists,
      .rows_from = source,
      .rows_query = *std::move(rows),
      .copies_table = true,
      .keeps_rounding_modes = true,
  };
  return (if_not_exists ? "CREATE TABLE IF NOT EXISTS " : "CREATE TABLE ") + *path + " (" +
         *columns + ")";
}

// The query's columns are cast to the declared types, which DuckDB would otherwise infer.
// CreateTableWrite applies top-level NOT NULL constraints in the same transaction as the query.
std::optional<std::string> CreateTableAsSelect(
    const googlesql::ResolvedCreateTableAsSelectStmt& create, const Scope& scope) {
  if (create.output_column_list_size() != create.column_definition_list_size()) {
    return Unsupported(scope, "CREATE TABLE AS SELECT columns");
  }
  const bool like = create.like_table() != nullptr;
  const auto head = CreateTableHead(create, scope);
  const std::optional<TableReference> target = scope.context.statement.ddl_target_table;
  std::optional<TableDescription> description;
  if (like) {
    description = LikeDescription(create, scope);
  } else {
    auto metadata = OptionsMetadata(create.option_list(), DdlOptionContext::kCreateTable, scope);
    if (metadata && DefaultCollation(create.collation_name(), scope, metadata->default_collation) &&
        PartitioningAndClustering(create, *metadata, scope) &&
        PrimaryKey(create, *metadata, scope)) {
      description = TableDescription{.schema = {}, .metadata = *std::move(metadata)};
    }
  }
  if (!head || !target || !description) {
    return std::nullopt;
  }
  const auto relation = Scan(*create.query(), scope);
  if (!relation) {
    return std::nullopt;
  }
  TableDefinition table{
      .table = *target,
      .schema = std::move(description->schema),
      .metadata = std::move(description->metadata),
      .if_not_exists =
          (create.create_mode() == googlesql::ResolvedCreateStatement::CREATE_IF_NOT_EXISTS),
      .as_select = true,
      .temporary = create.create_scope() == googlesql::ResolvedCreateStatement::CREATE_TEMP,
      .copies_table = like,
      .keeps_rounding_modes = like,
  };
  RoundingMode rounding_mode = table.metadata.default_rounding_mode;
  if (rounding_mode == RoundingMode::kUnspecified && !like && !table.temporary &&
      scope.context.defaults.source != nullptr) {
    if (const auto dataset = scope.context.defaults.source->DescribeDataset(target->project_id,
                                                                            target->dataset_id)) {
      rounding_mode = dataset->default_rounding_mode;
    }
  }
  std::vector<std::string> projections;
  for (int i = 0; i < create.output_column_list_size(); ++i) {
    const auto& definition = *create.column_definition_list(i);
    const auto column = relation->columns.find(create.output_column_list(i)->column().column_id());
    if (column == relation->columns.end()) {
      return std::nullopt;
    }
    std::optional<std::string> type;
    if (like) {
      const auto inherited_type = DuckDbColumnType(table.schema.at(i));
      if (!inherited_type.ok()) {
        return Unsupported(scope, inherited_type.status().message());
      }
      type = *inherited_type;
    } else {
      auto field = ColumnField(definition, scope);
      if (!field) {
        return std::nullopt;
      }
      CheckFieldNames(*field, field->name);
      type = ColumnDefinitionType(definition, scope);
      if (!type) {
        return std::nullopt;
      }
      table.schema.push_back(*std::move(field));
    }
    // The query's values are rounded with the rounding mode a column takes from the table's
    // default, or the dataset's, as CreateTableWrite records it.
    FieldSchema& field = table.schema.at(i);
    ApplyDefaultRoundingMode(field, rounding_mode);
    const auto value = WrittenValue(scope.context.functions, column->second,
                                    create.output_column_list(i)->column().type(), field);
    if (!value) {
      return std::nullopt;
    }
    projections.push_back("CAST(" + *value + " AS " + *type + ") AS " +
                          QuoteIdentifier(field.name));
  }
  scope.context.statement.table = std::move(table);
  return *head + " AS SELECT " + Join(projections, ", ") + relation->From() + relation->Order();
}

}  // namespace bigquery_emulator_duckdb::translator
