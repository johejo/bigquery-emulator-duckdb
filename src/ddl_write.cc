#include "src/ddl_write.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "absl/time/time.h"
#include "googlesql/public/value.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/column_default.h"
#include "src/column_metadata.h"
#include "src/duckdb_catalog.h"
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
    metadata.label_order = updates.label_order;
  }
  if (updates.resource_tags.has_value()) {
    metadata.resource_tags = *updates.resource_tags;
    metadata.tag_order = updates.tag_order;
  }
  for (const auto& [key, value] : updates.removed_tags) {
    const auto tag = metadata.resource_tags.find(key);
    if (tag != metadata.resource_tags.end() && tag->second == value) {
      metadata.resource_tags.erase(tag);
      std::erase(metadata.tag_order, key);
    }
  }
  if (updates.default_rounding_mode.has_value()) {
    metadata.default_rounding_mode = *updates.default_rounding_mode;
  }
}

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// The column of `schema` named `name`; BigQuery's column names ignore case, as DuckDB's do.
std::vector<FieldSchema>::iterator FindColumn(std::vector<FieldSchema>& schema,
                                              const std::string& name) {
  return std::ranges::find_if(schema, [&](const FieldSchema& field) {
    return ToLowerAscii(field.name) == ToLowerAscii(name);
  });
}

}  // namespace

DdlWrite CreateDatasetWrite(const DatasetDefinition& definition, const std::string& location) {
  DatasetMetadata metadata = definition.metadata;
  metadata.creation_time = NowMillis();
  if (metadata.location.empty()) {
    metadata.location = location;
  }
  DdlWrite write{
      .metadata_statements = DatasetMetadataStatements(definition.dataset, metadata),
  };
  if (definition.if_not_exists) {
    write.skip_query = "SELECT 1 WHERE " + DatasetExists(definition.dataset);
  }
  return write;
}

std::vector<std::string> DatasetMetadataStatements(const DatasetReference& dataset,
                                                   const DatasetMetadata& metadata) {
  ValidateLabels(metadata.labels);
  ValidateResourceTags(metadata.resource_tags);
  ValidateDefaultTableExpiration(metadata.default_table_expiration_ms);
  const std::string table = DatasetMetadataTable(dataset.project_id);
  std::vector<std::string> statements = {
      std::format("DELETE FROM {} WHERE dataset_id = {}", table, QuoteLiteral(dataset.dataset_id)),
  };
  if (!metadata.empty()) {
    nlohmann::json recorded = metadata.ToJson();
    if (!metadata.tag_order.empty()) {
      recorded["tagOrder"] = metadata.tag_order;
    }
    if (!metadata.label_order.empty()) {
      recorded["labelOrder"] = metadata.label_order;
    }
    statements.push_back(std::format("INSERT INTO {} VALUES ({}, {})", table,
                                     QuoteLiteral(dataset.dataset_id),
                                     QuoteLiteral(recorded.dump())));
  }
  return statements;
}

// DROP SCHEMA IF EXISTS of a missing dataset forgets nothing.
DdlWrite DropDatasetWrite(const DatasetReference& dataset) {
  return {
      .metadata_statements =
          {
              DatasetMetadataStatements(dataset, {}).front(),
              "DELETE FROM " + PropertyGraphsTable(dataset.project_id) +
                  " WHERE dataset_id = " + QuoteLiteral(dataset.dataset_id),
          },
  };
}

DdlWrite CreateViewWrite(const ViewDefinition& view, const DatasetMetadata& dataset) {
  const TableReference& table = view.table;
  TableMetadata metadata = view.metadata;
  // A view tables.update replaces keeps the creation time it is given.
  if (metadata.creation_time == 0) {
    metadata.creation_time = NowMillis();
  }
  metadata.expiration_time =
      ExpirationWithDefault(metadata.expiration_time, metadata.creation_time, dataset);
  DdlWrite write;
  // DuckDB can create a circular view and only reject it when queried. Bind the new definition
  // before committing so a failed replacement keeps the old view.
  write.metadata_statements = {
      "SELECT * FROM " + QualifiedName(table) + " LIMIT 0",
      ViewCommentStatement(table,
                           {
                               .query = view.query,
                               .schema = view.schema,
                               .metadata = metadata,
                               .materialized = view.materialized,
                               .materialized_source = view.materialized_source,
                           }),
  };
  if (view.if_not_exists) {
    write.skip_query = "SELECT 1 WHERE " + TableExists(table);
  }
  return write;
}

DdlWrite CreateTableWrite(const TableDefinition& definition, const DatasetMetadata& dataset) {
  if (definition.temporary && !definition.metadata.resource_tags.empty()) {
    throw ApiError::Unsupported("tags on temporary tables");
  }
  TableMetadata metadata = definition.metadata;
  metadata.creation_time = NowMillis();
  // A clone or snapshot made without time travel was made when it was created.
  for (std::optional<BaseTableDefinition>* made : {&metadata.clone, &metadata.snapshot}) {
    if (made->has_value() && (*made)->time.empty()) {
      (*made)->time =
          absl::FormatTime("%Y-%m-%dT%H:%M:%E3SZ", absl::FromUnixMillis(metadata.creation_time),
                           absl::UTCTimeZone());
    }
  }
  std::vector<FieldSchema> schema = definition.schema;
  if (!definition.temporary) {
    metadata.expiration_time =
        ExpirationWithDefault(metadata.expiration_time, metadata.creation_time, dataset);
    // CREATE TABLE COPY and CLONE keep the source's default rounding mode even when it is unset,
    // and LIKE takes the dataset's then, as BigQuery answers.
    if (metadata.default_rounding_mode.empty() && !definition.rows_from.has_value()) {
      metadata.default_rounding_mode = dataset.default_rounding_mode;
    }
  }
  if (!definition.keeps_rounding_modes) {
    ApplyDefaultRoundingMode(schema, metadata.default_rounding_mode);
  }
  // CREATE TABLE AS SELECT rounds the values it writes with the modes its columns have when it is
  // translated, which miss a dataset's default set since then.
  for (size_t i = 0; definition.as_select && i < schema.size(); ++i) {
    if (RoundsToEven(schema.at(i)) && !RoundsToEven(definition.schema.at(i))) {
      throw ApiError::Unsupported(
          "CREATE TABLE AS SELECT into parameterized columns that take ROUND_HALF_EVEN from the "
          "dataset");
    }
  }
  DdlWrite write{
      .metadata_statements = ColumnCommentStatements(definition.table, schema),
  };
  if (definition.as_select) {
    for (const FieldSchema& field : schema) {
      if (field.mode == FieldMode::kRequired) {
        write.metadata_statements.push_back("ALTER TABLE " + QualifiedName(definition.table) +
                                            " ALTER COLUMN " + QuoteIdentifier(field.name) +
                                            " SET NOT NULL");
      }
    }
  }
  std::ranges::move(RepeatedColumnDefaultStatements(definition.table, schema),
                    std::back_inserter(write.metadata_statements));
  if (!metadata.empty()) {
    write.metadata_statements.push_back(TableCommentStatement(definition.table, metadata, schema));
  }
  if (definition.rows_from.has_value()) {
    write.metadata_statements.push_back(
        "INSERT INTO " + QualifiedName(definition.table) + " " +
        definition.rows_query.value_or("SELECT * FROM " + QualifiedName(*definition.rows_from)));
  }
  if (definition.if_not_exists) {
    write.skip_query = "SELECT 1 WHERE " + TableExists(definition.table);
  }
  return write;
}

std::vector<std::string> AlterTableStatements(const TableAlteration& alteration,
                                              std::vector<FieldSchema>& schema,
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
      ApplyDefaultRoundingMode(schema.back(), metadata.default_rounding_mode);
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
      if (options->rounding_mode.has_value()) {
        column->rounding_mode = *options->rounding_mode;
      }
      commented.push_back(column->name);
    } else if (const auto* value = std::get_if<ColumnDefaultAction>(&action)) {
      const auto column = FindColumn(schema, value->name);
      if (column == schema.end()) {
        if (value->if_exists) {
          continue;
        }
        throw ApiError::Invalid("Column not found: " + value->name);
      }
      if (!value->drop) {
        ValidateColumnDefault(
            *column, value->literal != nullptr ? std::optional(*value->literal) : std::nullopt);
      }
      if (value->drop && column->default_value_expression.empty()) {
        throw ApiError::Invalid("Column has no default value: " + column->name);
      }
      statements.push_back(std::format("ALTER TABLE {} ALTER COLUMN {} {}", QualifiedName(table),
                                       QuoteIdentifier(column->name),
                                       value->drop ? "DROP DEFAULT" : "SET DEFAULT " + value->sql));
      column->default_value_expression =
          value->literal != nullptr && value->literal->is_null() ? "" : value->expression;
      // REPEATED columns without a declared default still default to an empty array.
      std::ranges::move(RepeatedColumnDefaultStatements(table, {*column}),
                        std::back_inserter(statements));
      commented.push_back(column->name);
    } else {
      const auto& updates = std::get<OptionUpdates>(action);
      ApplyOptionUpdates(updates, metadata);
      if (updates.require_partition_filter.has_value()) {
        metadata.require_partition_filter = *updates.require_partition_filter;
      }
      if (updates.expiration_time.has_value()) {
        metadata.expiration_time = *updates.expiration_time;
      }
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
  if (alteration.options.default_table_expiration_ms.has_value()) {
    metadata.default_table_expiration_ms = *alteration.options.default_table_expiration_ms;
  }
  return DatasetMetadataStatements(alteration.dataset, metadata);
}

std::optional<DdlWrite> MetadataWrite(const TranslatedStatement& statement,
                                      const DatasetMetadata& dataset, const std::string& location) {
  if (statement.table.has_value()) {
    return CreateTableWrite(*statement.table, dataset);
  }
  if (statement.view.has_value()) {
    return CreateViewWrite(*statement.view, dataset);
  }
  if (statement.dataset.has_value()) {
    return CreateDatasetWrite(*statement.dataset, location);
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
