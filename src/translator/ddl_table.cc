#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/catalog.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/references.h"
#include "src/table_metadata.h"
#include "src/translated_statement.h"
#include "src/translator/context.h"
#include "src/translator/ddl.h"
#include "src/translator/ddl_internal.h"
#include "src/translator/expression.h"
#include "src/translator/scan.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

// The temporary table CREATE TEMP TABLE `path` creates. The analyzer rejects one outside a
// multi-statement query, and one with a qualified name.
std::optional<std::string> TemporaryTargetTable(const std::vector<std::string>& path,
                                                const Scope& scope) {
  const TemporaryTables* temporary = scope.context.defaults.temporary;
  const std::optional<std::string> name = TemporaryTableName(path);
  if (temporary == nullptr || !name) {
    return Unsupported(scope, "temporary table " + Join(path, "."));
  }
  scope.context.ddl_target_table = TableReference{temporary->project, temporary->dataset, *name};
  return QualifiedName(*scope.context.ddl_target_table);
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
    metadata.time_partitioning = TimePartitioning{.type = "DAY", .field = column};
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
    metadata.time_partitioning = TimePartitioning{.type = "DAY", .field = column};
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
  metadata.time_partitioning = TimePartitioning{.type = part, .field = column};
  return part == "MONTH" || part == "YEAR" ||
         (!type->IsDate() && (part == "DAY" || part == "HOUR"));
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

// BigQuery's primary and foreign keys are never enforced, so they are dropped.
std::optional<std::string> CreateTableHead(const googlesql::ResolvedCreateTableStmtBase& create,
                                           const Scope& scope) {
  if (create.is_value_table() || !create.pseudo_column_list().empty() ||
      create.collation_name() != nullptr || create.connection_list() != nullptr ||
      !create.check_constraint_list().empty()) {
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
      return "CREATE OR REPLACE TABLE " + *path;
    case googlesql::ResolvedCreateStatement::CREATE_IF_NOT_EXISTS:
      return "CREATE TABLE IF NOT EXISTS " + *path;
    default:
      return "CREATE TABLE " + *path;
  }
}

// The schema and metadata of `table`, the source of `statement`, which is a CREATE TABLE LIKE,
// COPY or CLONE. A clone's cloneDefinition is its own, which no copy inherits.
std::optional<TableDescription> SourceDescription(const googlesql::Table* table,
                                                  std::string_view statement, const Scope& scope) {
  const auto* source = dynamic_cast<const BigQueryTable*>(table);
  std::optional<TableDescription> description =
      source == nullptr ? std::nullopt : source->Describe();
  if (!description) {
    Unsupported(scope, std::string(statement) + " a view");
    return std::nullopt;
  }
  // A default is stored as GoogleSQL, which would have to be translated again.
  if (std::ranges::any_of(description->schema, [](const FieldSchema& field) {
        return !field.default_value_expression.empty();
      })) {
    Unsupported(scope, std::string(statement) + " a table with column defaults");
    return std::nullopt;
  }
  description->metadata.clone.reset();
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

// CREATE TABLE LIKE copies the schema, partitioning, clustering, description, friendly name and
// labels of the source table, which PARTITION BY, CLUSTER BY and OPTIONS replace, as ALTER TABLE
// SET OPTIONS does.
std::optional<std::string> CreateTableLike(const googlesql::ResolvedCreateTableStmt& create,
                                           const Scope& scope) {
  std::optional<TableDescription> description =
      SourceDescription(create.like_table(), "CREATE TABLE LIKE", scope);
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
  const auto head = CreateTableHead(create, scope);
  const std::optional<TableReference> target = scope.context.ddl_target_table;
  auto metadata =
      OptionsMetadata(create.option_list(), "CREATE TABLE", scope, std::move(inherited));
  if (!head || !target || !metadata || !PartitioningAndClustering(create, *metadata, scope)) {
    return std::nullopt;
  }
  const auto columns = ColumnsSql(description->schema, scope);
  if (!columns) {
    return std::nullopt;
  }
  scope.context.table = TableDefinition{
      .table = *target,
      .schema = std::move(description->schema),
      .metadata = *std::move(metadata),
      .if_not_exists = IfNotExists(create),
  };
  return *head + " (" + *columns + ")";
}

// The time now in RFC 3339, as tables.get reports a cloneTime.
std::string Rfc3339Now() {
  return absl::FormatTime("%Y-%m-%dT%H:%M:%E3SZ", absl::Now(), absl::UTCTimeZone());
}

// CREATE TABLE COPY and CLONE copy the rows of the source table besides what CREATE TABLE LIKE
// copies, which OPTIONS replace; the analyzer rejects PARTITION BY and CLUSTER BY with them. A
// clone also records the table it was cloned from and when, which tables.get reports as its
// cloneDefinition. The emulator keeps no table's history, so FOR SYSTEM_TIME AS OF is
// unsupported, and so are a temporary copy or clone and CREATE OR REPLACE of a clone, which
// BigQuery does not document.
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
  if (scan.for_system_time_expr() != nullptr) {
    return Unsupported(scope, statement + " with FOR SYSTEM_TIME AS OF");
  }
  if (create.create_scope() == googlesql::ResolvedCreateStatement::CREATE_TEMP) {
    return Unsupported(scope, "CREATE TEMP TABLE " + kind);
  }
  const bool replace =
      create.create_mode() == googlesql::ResolvedCreateStatement::CREATE_OR_REPLACE;
  if (clone && replace) {
    return Unsupported(scope, "CREATE OR REPLACE TABLE CLONE");
  }
  std::optional<TableDescription> description = SourceDescription(scan.table(), statement, scope);
  if (!description) {
    return std::nullopt;
  }
  const TableReference source = dynamic_cast<const BigQueryTable*>(scan.table())->reference();
  const auto head = CreateTableHead(create, scope);
  const std::optional<TableReference> target = scope.context.ddl_target_table;
  if (clone) {
    description->metadata.clone = CloneDefinition{.base_table = source, .clone_time = Rfc3339Now()};
  }
  auto metadata = OptionsMetadata(create.option_list(), "CREATE TABLE", scope,
                                  std::move(description->metadata));
  const auto columns = ColumnsSql(description->schema, scope);
  if (!head || !target || !metadata || !columns) {
    return std::nullopt;
  }
  // The new table would replace the source before its rows are copied.
  if (replace && ToLowerAscii(QualifiedName(*target)) == ToLowerAscii(QualifiedName(source))) {
    return Unsupported(scope, "CREATE OR REPLACE TABLE COPY of itself");
  }
  scope.context.table = TableDefinition{
      .table = *target,
      .schema = std::move(description->schema),
      .metadata = *std::move(metadata),
      .if_not_exists = IfNotExists(create),
      .rows_from = source,
  };
  return *head + " (" + *columns + ")";
}

}  // namespace

// ALTER TABLE ADD COLUMN, DROP COLUMN, RENAME TO, ALTER COLUMN SET OPTIONS and SET OPTIONS, which
// the emulator applies to the table as it is when the statement runs.
std::optional<std::string> AlterTable(const googlesql::ResolvedAlterTableStmt& alter,
                                      const Scope& scope) {
  const auto path = TargetTable(alter.name_path(), scope);
  const std::optional<TableReference> target = scope.context.ddl_target_table;
  if (!path || !target) {
    return std::nullopt;
  }
  TableAlteration alteration{.table = *target, .if_exists = alter.is_if_exists()};
  for (const auto& action : alter.alter_action_list()) {
    // BigQuery documents a list of ADD COLUMN or of DROP COLUMN actions, but not a mix of kinds.
    if (action->node_kind() != alter.alter_action_list(0)->node_kind()) {
      return Unsupported(scope, "ALTER TABLE with different kinds of actions");
    }
    if (action->Is<googlesql::ResolvedAddColumnAction>()) {
      const auto& add = *action->GetAs<googlesql::ResolvedAddColumnAction>();
      const auto& column = *add.column_definition();
      if (column.is_hidden() || column.generated_column_info() != nullptr ||
          column.default_value() != nullptr ||
          (column.annotations() != nullptr && column.annotations()->not_null())) {
        return Unsupported(scope, "ADD COLUMN with generated columns, defaults or NOT NULL");
      }
      auto field = ColumnField(column, scope);
      if (!ColumnDefinitionType(column, scope) || !field) {
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
    } else if (action->Is<googlesql::ResolvedSetOptionsAction>()) {
      OptionUpdates updates;
      if (!SetOptions(action->GetAs<googlesql::ResolvedSetOptionsAction>()->option_list(),
                      "ALTER TABLE", scope, updates)) {
        return std::nullopt;
      }
      alteration.actions.emplace_back(std::move(updates));
    } else {
      return Unsupported(scope, "ALTER TABLE action " + action->node_kind_string());
    }
  }
  scope.context.altered_table = std::move(alteration);
  return "";
}

// The analyzer gives an ALTER TABLE with only SET OPTIONS actions as this statement.
std::optional<std::string> AlterTableSetOptions(
    const googlesql::ResolvedAlterTableSetOptionsStmt& alter, const Scope& scope) {
  const auto path = TargetTable(alter.name_path(), scope);
  const std::optional<TableReference> target = scope.context.ddl_target_table;
  OptionUpdates updates;
  if (!path || !target || !SetOptions(alter.option_list(), "ALTER TABLE", scope, updates)) {
    return std::nullopt;
  }
  scope.context.altered_table = TableAlteration{
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
  const std::optional<TableReference> target = scope.context.ddl_target_table;
  auto metadata = OptionsMetadata(create.option_list(), "CREATE TABLE", scope);
  if (!head || !target || !metadata || !PartitioningAndClustering(create, *metadata, scope)) {
    return std::nullopt;
  }
  TableDefinition table{
      .table = *target,
      .metadata = *std::move(metadata),
      .if_not_exists = IfNotExists(create),
  };
  std::vector<std::string> columns;
  for (const auto& column : create.column_definition_list()) {
    if (column->is_hidden() || column->generated_column_info() != nullptr) {
      return Unsupported(scope, "generated columns");
    }
    const auto type = ColumnDefinitionType(*column, scope);
    auto field = ColumnField(*column, scope);
    if (!type || !field) {
      return std::nullopt;
    }
    table.schema.push_back(*std::move(field));
    std::string sql = QuoteIdentifier(column->name()) + " " + *type;
    if (column->annotations() != nullptr && column->annotations()->not_null()) {
      sql += " NOT NULL";
    }
    if (column->default_value() != nullptr) {
      // A parameterized column's default is cast to its type with the parameters, which the
      // column itself applies when the default is stored.
      const googlesql::ResolvedExpr* expression = column->default_value()->expression();
      if (expression->Is<googlesql::ResolvedCast>()) {
        const auto* cast = expression->GetAs<googlesql::ResolvedCast>();
        if (cast->format() == nullptr && cast->time_zone() == nullptr &&
            cast->extended_cast() == nullptr && !cast->return_null_on_error()) {
          expression = cast->expr();
        }
      }
      const auto value = Expression(*expression, scope, {});
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
  scope.context.table = std::move(table);
  return *head + " (" + Join(columns, ", ") + ")";
}

// The query's columns are cast to the declared types, which DuckDB would otherwise infer.
std::optional<std::string> CreateTableAsSelect(
    const googlesql::ResolvedCreateTableAsSelectStmt& create, const Scope& scope) {
  if (create.output_column_list_size() != create.column_definition_list_size()) {
    return Unsupported(scope, "CREATE TABLE AS SELECT columns");
  }
  if (create.like_table() != nullptr) {
    return Unsupported(scope, "CREATE TABLE LIKE AS SELECT");
  }
  const auto head = CreateTableHead(create, scope);
  const std::optional<TableReference> target = scope.context.ddl_target_table;
  auto metadata = OptionsMetadata(create.option_list(), "CREATE TABLE", scope);
  if (!head || !target || !metadata || !PartitioningAndClustering(create, *metadata, scope)) {
    return std::nullopt;
  }
  const auto relation = Scan(*create.query(), scope);
  if (!relation) {
    return std::nullopt;
  }
  TableDefinition table{
      .table = *target,
      .metadata = *std::move(metadata),
      .if_not_exists = IfNotExists(create),
  };
  std::vector<std::string> projections;
  for (int i = 0; i < create.output_column_list_size(); ++i) {
    const auto& definition = *create.column_definition_list(i);
    // DuckDB cannot declare constraints on a table created from a query.
    if (definition.annotations() != nullptr && definition.annotations()->not_null()) {
      return Unsupported(scope, "NOT NULL in CREATE TABLE AS SELECT");
    }
    const auto column = relation->columns.find(create.output_column_list(i)->column().column_id());
    const auto type = ColumnDefinitionType(definition, scope);
    auto field = ColumnField(definition, scope);
    if (column == relation->columns.end() || !type || !field) {
      return std::nullopt;
    }
    table.schema.push_back(*std::move(field));
    projections.push_back("CAST(" + column->second + " AS " + *type + ") AS " +
                          QuoteIdentifier(definition.name()));
  }
  scope.context.table = std::move(table);
  return *head + " AS SELECT " + Join(projections, ", ") + relation->From() + relation->Order();
}

}  // namespace bigquery_emulator_duckdb::translator
