#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/ddl_write.h"
#include "src/duckdb_catalog.h"
#include "src/function_catalog/catalog.h"
#include "src/location.h"
#include "src/references.h"
#include "src/sql_runtime.h"
#include "src/table_comments.h"
#include "src/table_history.h"
#include "src/table_metadata.h"
#include "src/translated_statement.h"

namespace bigquery_emulator_duckdb {

using nlohmann::json;

void SqlRuntime::CheckDdlTarget(const TranslatedStatement& translation) {
  if (const auto& target = translation.ddl_target_table;
      target.has_value() && IsDuckDbSchema(target->dataset_id)) {
    throw ApiError::NotFound("Not found: Dataset " + target->project_id + ":" + target->dataset_id);
  }
  if (const auto& target = translation.ddl_target_dataset;
      target.has_value() && IsDuckDbSchema(target->dataset_id)) {
    throw ApiError::NotFound("Not found: Dataset " + target->project_id + ":" + target->dataset_id);
  }
  if (const auto& target = translation.ddl_target_routine;
      target.has_value() && IsDuckDbSchema(target->dataset_id)) {
    throw ApiError::NotFound("Not found: Dataset " + target->project_id + ":" + target->dataset_id);
  }
}

void SqlRuntime::CheckRelationTarget(Backend& backend, const TranslatedStatement& translation) {
  const std::optional<TableReference>& target = translation.ddl_target_table;
  if (!target.has_value()) {
    return;
  }
  const auto view_comment = ViewComment(backend, *target);
  const auto view = view_comment ? ParseViewMetadata(*view_comment) : std::nullopt;
  const bool materialized = view && view->materialized;
  if (translation.statement_type == "DROP_VIEW" && materialized) {
    throw ApiError::Invalid("Cannot drop " + TableName(*target) +
                            " which has type MATERIALIZED_VIEW. A view was expected.");
  }
  if ((translation.statement_type == "DROP_MATERIALIZED_VIEW" && view_comment && !materialized) ||
      (translation.replaces && materialized)) {
    throw ApiError::InvalidQuery("Object type does not match " + TableName(*target));
  }
  const auto comment = RelationComment(backend, "duckdb_tables()", "table_name", *target);
  if (translation.statement_type == "DROP_MATERIALIZED_VIEW" && comment.has_value()) {
    throw ApiError::InvalidQuery("A materialized view was expected: " + TableName(*target));
  }
  // DROP EXTERNAL TABLE drops only an external table, even with IF EXISTS.
  const bool drops_external = translation.statement_type == "DROP_EXTERNAL_TABLE";
  if (drops_external && view_comment) {
    throw ApiError::Invalid("Cannot drop " + TableName(*target) + " which has type " +
                            (materialized ? "MATERIALIZED_VIEW" : "VIEW") +
                            ". An external table was expected.");
  }
  if (!comment.has_value()) {
    // The translation of DROP EXTERNAL TABLE IF EXISTS keeps its IF EXISTS.
    if (drops_external && !translation.sql.starts_with("DROP TABLE IF EXISTS ")) {
      throw ApiError::NotFound("Not found: External table " + TableName(*target));
    }
    return;
  }
  const bool external = CommentMetadata(*comment).external_data.has_value();
  const bool snapshot = CommentMetadata(*comment).snapshot.has_value();
  if (drops_external && !external) {
    throw ApiError::Invalid("Cannot drop " + TableName(*target) + " which has type " +
                            (snapshot ? "SNAPSHOT" : "TABLE") +
                            ". An external table was expected.");
  }
  if (external && translation.altered_table) {
    throw ApiError::Unsupported("ALTER TABLE on external tables");
  }
  const bool creates_external = translation.table && translation.table->metadata.external_data;
  if (translation.replaces && external && !creates_external) {
    throw ApiError::Invalid(TableName(*target) +
                            " is not allowed for this operation because it currently has type "
                            "EXTERNAL.");
  }
  if (translation.replaces && !external && !snapshot && creates_external) {
    throw ApiError::Invalid(TableName(*target) +
                            " is not allowed for this operation because it is currently a TABLE.");
  }
  if (translation.statement_type == "DROP_SNAPSHOT_TABLE" && external) {
    throw ApiError::Invalid("Cannot drop " + TableName(*target) +
                            " which has type EXTERNAL TABLE. A snapshot was expected.");
  }
  if (translation.statement_type == "DROP_SNAPSHOT_TABLE" && !snapshot) {
    throw ApiError::InvalidQuery("Invalid value: Cannot drop " + TableName(*target) +
                                 " which has type TABLE. A snapshot was expected.");
  }
  if (!snapshot) {
    return;
  }
  if (translation.statement_type == "DROP_TABLE") {
    throw ApiError::InvalidQuery("Invalid value: Cannot drop " + TableName(*target) +
                                 " which has type SNAPSHOT. A table was expected.");
  }
  if (const auto& alteration = translation.altered_table) {
    // BigQuery's messages for SET OPTIONS and for ADD and DROP COLUMN; what it reports for the
    // other actions is not known.
    const std::vector<TableAlterAction>& actions = alteration->actions;
    if (std::ranges::all_of(actions, [](const TableAlterAction& action) {
          return std::holds_alternative<OptionUpdates>(action);
        })) {
      throw ApiError::InvalidQuery(
          "Invalid value: ALTER TABLE SET OPTIONS only supports setting options for tables, "
          "while " +
          target->table_id + " is a(n) snapshot");
    }
    if (std::ranges::all_of(actions, [](const TableAlterAction& action) {
          return std::holds_alternative<AddColumnAction>(action) ||
                 std::holds_alternative<DropColumnAction>(action);
        })) {
      throw ApiError::InvalidQuery(
          "Invalid value: ALTER TABLE does not support modifying the schema of a(n) SNAPSHOT: " +
          target->table_id);
    }
    throw ApiError::Unsupported("this ALTER TABLE action on a table snapshot");
  }
  if (translation.replaces) {
    throw ApiError::InvalidQuery("Invalid value: " + TableName(*target) +
                                 " is not allowed for this operation because it currently has "
                                 "type SNAPSHOT.");
  }
}

// Throws DuckDB's `error` about the macro that records the routine a CREATE or DROP FUNCTION
// names in BigQuery's words, if it says that the routine exists or that it does not.
std::optional<std::vector<std::string>> SqlRuntime::AlterationStatements(
    const TranslatedStatement& translation) {
  if (const auto& alteration = translation.altered_table) {
    const TableReference& table = alteration->table;
    if (ViewComment(backend_, table).has_value()) {
      throw ApiError::Invalid("ALTER TABLE cannot alter view " + TableName(table));
    }
    const auto comment = RelationComment(backend_, "duckdb_tables()", "table_name", table);
    if (!comment.has_value()) {
      if (alteration->if_exists) {
        return std::vector<std::string>{};
      }
      throw ApiError::NotFound("Not found: Table " + TableName(table));
    }
    auto schema = TableSchema(backend_, table);
    const TableMetadata metadata = CommentMetadata(*comment);
    auto statements =
        AlterTableStatements(*alteration, schema, metadata, functions_.timestamp_precision());
    TableReference destination = table;
    bool discards_history = false;
    for (const auto& action : alteration->actions) {
      if (const auto* rename = std::get_if<RenameTableAction>(&action)) {
        destination.table_id = rename->table_id;
      }
      discards_history |= std::holds_alternative<AddColumnAction>(action) ||
                          std::holds_alternative<DropColumnAction>(action);
    }
    std::ranges::move(ColumnDefaultStatements(destination, schema), std::back_inserter(statements));
    // A table's history follows it to its new name, but the emulator does not keep the rows it
    // had in other columns.
    if (const int64_t creation_time = HistoryCreationTime(table, metadata);
        !statements.empty() && creation_time != 0) {
      if (destination.table_id != table.table_id) {
        std::ranges::move(RenameHistoryStatements(table, creation_time, destination.table_id),
                          std::back_inserter(statements));
      }
      if (discards_history) {
        std::ranges::move(DiscardHistoryStatements(destination, creation_time),
                          std::back_inserter(statements));
        statements.push_back(CommitVersionsStatement(table.project_id));
      }
    }
    return statements;
  }
  if (const auto& alteration = translation.altered_dataset) {
    const DatasetReference& dataset = alteration->dataset;
    const QueryResult result = Execute(DatasetEntriesQuery(dataset.project_id, dataset.dataset_id));
    if (result.rows.empty()) {
      if (alteration->if_exists) {
        return std::vector<std::string>{};
      }
      throw ApiError::NotFound("Not found: Dataset " + dataset.project_id + ":" +
                               dataset.dataset_id);
    }
    return AlterDatasetStatements(*alteration,
                                  ParseDatasetMetadata(result.rows.OptionalString(0, 1)));
  }
  return std::nullopt;
}

DatasetMetadata SqlRuntime::CreatedInDataset(Backend& backend,
                                             const TranslatedStatement& translation) {
  std::optional<TableReference> table;
  if (translation.table.has_value() && !translation.table->temporary) {
    table = translation.table->table;
  } else if (translation.view.has_value()) {
    table = translation.view->table;
  }
  if (!table.has_value()) {
    return {};
  }
  const QueryResult result =
      backend.Execute(DatasetEntriesQuery(table->project_id, table->dataset_id));
  return result.rows.empty() ? DatasetMetadata{}
                             : ParseDatasetMetadata(result.rows.OptionalString(0, 1));
}

std::optional<std::string> SqlRuntime::DatasetLocation(Backend& backend,
                                                       const DatasetReference& dataset) {
  return LocationOf(backend, dataset);
}

void SqlRuntime::UseTargets(Backend& backend, const TranslatedStatement& translation,
                            JobLocation& location) {
  const auto use = [&](const DatasetReference& dataset) {
    if (const std::optional<std::string> found = DatasetLocation(backend, dataset)) {
      location.Reference("Dataset " + dataset.project_id + ":" + dataset.dataset_id, *found);
    }
  };
  if (const auto& definition = translation.dataset) {
    // The location CREATE SCHEMA names does not settle the job's, as BigQuery answers, even for a
    // dataset that IF NOT EXISTS keeps.
    const std::string& named = definition->metadata.location;
    if (!named.empty() && named != location.Resolve()) {
      throw ApiError::InvalidQuery("Location specified in query " + named +
                                   " is not consistent with current execution region " +
                                   location.Resolve());
    }
  } else if (const auto& target = translation.ddl_target_dataset) {
    use(*target);
  }
  if (const auto& target = translation.ddl_target_table) {
    use({.project_id = target->project_id, .dataset_id = target->dataset_id});
  }
  if (const auto& target = translation.property_graph) {
    use({.project_id = target->project_id, .dataset_id = target->dataset_id});
  }
  if (const auto& target = translation.ddl_target_routine) {
    use({.project_id = target->project_id, .dataset_id = target->dataset_id});
  }
}

}  // namespace bigquery_emulator_duckdb
