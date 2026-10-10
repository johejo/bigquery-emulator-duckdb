#include "src/ddl_write.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/column_metadata.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/references.h"
#include "src/routine.h"
#include "src/schema_sql.h"
#include "src/table_comments.h"
#include "src/table_metadata.h"
#include "src/translated_statement.h"

namespace bigquery_emulator_duckdb {
namespace {

std::string TableExists(const TableReference& table) {
  return std::format(
      "EXISTS (SELECT 1 FROM information_schema.tables"
      " WHERE table_catalog = {} AND table_schema = {} AND table_name = {})",
      QuoteLiteral(table.project_id), QuoteLiteral(table.dataset_id), QuoteLiteral(table.table_id));
}

std::string DatasetExists(const DatasetReference& dataset) {
  return std::format(
      "EXISTS (SELECT 1 FROM information_schema.schemata"
      " WHERE catalog_name = {} AND schema_name = {})",
      QuoteLiteral(dataset.project_id), QuoteLiteral(dataset.dataset_id));
}

DdlWrite CreateDatasetWrite(const DatasetDefinition& definition) {
  DdlWrite write{
      .metadata_statements = DatasetMetadataStatements(definition.dataset, definition.metadata),
  };
  if (definition.if_not_exists) {
    write.skip_query = "SELECT 1 WHERE " + DatasetExists(definition.dataset);
  }
  return write;
}

// The options `updates` sets on the table's or dataset's `metadata`.
template <typename Metadata>
void ApplyOptionUpdates(const OptionUpdates& updates, Metadata& metadata) {
  if (updates.description.has_value()) {
    metadata.description = *updates.description;
  }
  if (updates.friendly_name.has_value()) {
    metadata.friendly_name = *updates.friendly_name;
  }
  if (updates.labels.has_value()) {
    metadata.labels = *updates.labels;
  }
}

// The column of `schema` named `name`; BigQuery's column names ignore case, as DuckDB's do.
std::vector<FieldSchema>::iterator FindColumn(std::vector<FieldSchema>& schema,
                                              const std::string& name) {
  return std::ranges::find_if(schema, [&](const FieldSchema& field) {
    return ToLowerAscii(field.name) == ToLowerAscii(name);
  });
}

}  // namespace

std::string DatasetMetadataTable(const std::string& project) {
  return QuoteIdentifier(project) + ".main.emulator_datasets";
}

std::vector<std::string> DatasetMetadataStatements(const DatasetReference& dataset,
                                                   const DatasetMetadata& metadata) {
  ValidateLabels(metadata.labels);
  const std::string table = DatasetMetadataTable(dataset.project_id);
  std::vector<std::string> statements = {
      std::format("DELETE FROM {} WHERE dataset_id = {}", table, QuoteLiteral(dataset.dataset_id)),
  };
  if (!metadata.empty()) {
    statements.push_back(std::format("INSERT INTO {} VALUES ({}, {})", table,
                                     QuoteLiteral(dataset.dataset_id),
                                     QuoteLiteral(metadata.ToJson().dump())));
  }
  return statements;
}

// DROP SCHEMA IF EXISTS of a missing dataset forgets nothing.
DdlWrite DropDatasetWrite(const DatasetReference& dataset) {
  return {.metadata_statements = {DatasetMetadataStatements(dataset, {}).front()}};
}

DdlWrite CreateViewWrite(const ViewDefinition& view) {
  const TableReference& table = view.table;
  DdlWrite write;
  // DuckDB can create a circular view and only reject it when queried. Bind the new definition
  // before committing so a failed replacement keeps the old view.
  write.metadata_statements = {
      "SELECT * FROM " + QualifiedName(table) + " LIMIT 0",
      ViewCommentStatement(table,
                           {.query = view.query, .schema = view.schema, .metadata = view.metadata}),
  };
  if (view.if_not_exists) {
    write.skip_query = "SELECT 1 WHERE " + TableExists(table);
  }
  return write;
}

DdlWrite CreateTableWrite(const TableDefinition& definition) {
  DdlWrite write{
      .metadata_statements = ColumnCommentStatements(definition.table, definition.schema),
  };
  std::ranges::move(RepeatedColumnDefaultStatements(definition.table, definition.schema),
                    std::back_inserter(write.metadata_statements));
  if (!definition.metadata.empty()) {
    write.metadata_statements.push_back(
        TableCommentStatement(definition.table, definition.metadata, definition.schema));
  }
  if (definition.rows_from.has_value()) {
    write.metadata_statements.push_back("INSERT INTO " + QualifiedName(definition.table) +
                                        " SELECT * FROM " + QualifiedName(*definition.rows_from));
  }
  if (definition.if_not_exists) {
    write.skip_query = "SELECT 1 WHERE " + TableExists(definition.table);
  }
  return write;
}

std::vector<std::string> AlterTableStatements(const TableAlteration& alteration,
                                              std::vector<FieldSchema> schema,
                                              TableMetadata metadata) {
  TableReference table = alteration.table;
  std::vector<std::string> statements;
  // The columns whose comments the actions change, which are written once the schema is final.
  std::vector<std::string> commented;
  bool set_options = false;
  for (const TableAlterAction& action : alteration.actions) {
    if (const auto* add = std::get_if<AddColumnAction>(&action)) {
      if (FindColumn(schema, add->field.name) != schema.end()) {
        if (add->if_not_exists) {
          continue;
        }
        throw ApiError::Invalid("Column already exists: " + add->field.name);
      }
      // Existing rows read an added REPEATED column as empty, as rows that leave it out later do;
      // see RepeatedColumnDefaultStatements.
      statements.push_back(std::format(
          "ALTER TABLE {} ADD COLUMN {}{}", QualifiedName(table), ColumnDefinition(add->field),
          add->field.mode == FieldMode::kRepeated ? " DEFAULT []" : ""));
      schema.push_back(add->field);
      commented.push_back(add->field.name);
    } else if (const auto* drop = std::get_if<DropColumnAction>(&action)) {
      const auto column = FindColumn(schema, drop->name);
      if (column == schema.end()) {
        if (drop->if_exists) {
          continue;
        }
        throw ApiError::Invalid("Column not found: " + drop->name);
      }
      const auto names = [&](const std::string& field) {
        return ToLowerAscii(field) == ToLowerAscii(column->name);
      };
      if ((metadata.time_partitioning.has_value() && names(metadata.time_partitioning->field)) ||
          (metadata.range_partitioning.has_value() && names(metadata.range_partitioning->field))) {
        throw ApiError::Invalid("Cannot drop partitioning column " + column->name);
      }
      if (std::ranges::any_of(metadata.clustering, names)) {
        throw ApiError::Invalid("Cannot drop clustering column " + column->name);
      }
      statements.push_back(std::format("ALTER TABLE {} DROP COLUMN {}", QualifiedName(table),
                                       QuoteIdentifier(column->name)));
      schema.erase(column);
    } else if (const auto* rename = std::get_if<RenameTableAction>(&action)) {
      statements.push_back(std::format("ALTER TABLE {} RENAME TO {}", QualifiedName(table),
                                       QuoteIdentifier(rename->table_id)));
      table.table_id = rename->table_id;
    } else if (const auto* options = std::get_if<ColumnOptionsAction>(&action)) {
      const auto column = FindColumn(schema, options->name);
      if (column == schema.end()) {
        if (options->if_exists) {
          continue;
        }
        throw ApiError::Invalid("Column not found: " + options->name);
      }
      if (options->description.has_value()) {
        column->description = *options->description;
      }
      commented.push_back(column->name);
    } else {
      ApplyOptionUpdates(std::get<OptionUpdates>(action), metadata);
      set_options = true;
    }
  }
  for (const std::string& name : commented) {
    if (const auto column = FindColumn(schema, name); column != schema.end()) {
      std::ranges::move(ColumnCommentStatements(table, {*column}), std::back_inserter(statements));
    }
  }
  if (set_options) {
    statements.push_back(TableCommentStatement(table, metadata, schema));
  }
  return statements;
}

std::vector<std::string> AlterDatasetStatements(const DatasetAlteration& alteration,
                                                DatasetMetadata metadata) {
  ApplyOptionUpdates(alteration.options, metadata);
  return DatasetMetadataStatements(alteration.dataset, metadata);
}

std::optional<DdlWrite> MetadataWrite(const TranslatedStatement& statement) {
  if (statement.table.has_value()) {
    return CreateTableWrite(*statement.table);
  }
  if (statement.view.has_value()) {
    return CreateViewWrite(*statement.view);
  }
  if (statement.dataset.has_value()) {
    return CreateDatasetWrite(*statement.dataset);
  }
  if (statement.statement_type == "DROP_SCHEMA" && statement.ddl_target_dataset.has_value()) {
    return DropDatasetWrite(*statement.ddl_target_dataset);
  }
  if (const auto& definition = statement.routine) {
    return DdlWrite{
        .metadata_statements = RoutineCommentStatements(definition->routine),
        .skip_query = definition->if_not_exists ? RoutineQuery(definition->routine.reference) : "",
    };
  }
  return std::nullopt;
}

}  // namespace bigquery_emulator_duckdb
