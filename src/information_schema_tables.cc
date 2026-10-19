#include "src/information_schema_tables.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_join.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/type.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/information_schema_options.h"
#include "src/information_schema_rows.h"
#include "src/information_schema_view.h"
#include "src/references.h"
#include "src/table_metadata.h"
#include "src/table_source.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::information_schema {

namespace {

absl::StatusOr<std::string> TypeName(const FieldSchema& field,
                                     googlesql::TypeFactory* type_factory) {
  absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(field, type_factory);
  if (!type.ok()) {
    return type.status();
  }
  return (*type)->TypeName(googlesql::PRODUCT_EXTERNAL);
}

absl::StatusOr<std::string> TableDdl(const std::string& project, const std::string& dataset,
                                     const std::string& table,
                                     const std::vector<FieldSchema>& schema,
                                     googlesql::TypeFactory* type_factory) {
  std::vector<std::string> columns;
  for (const FieldSchema& field : schema) {
    absl::StatusOr<std::string> type = TypeName(field, type_factory);
    if (!type.ok()) {
      return type.status();
    }
    columns.push_back("  " + field.name + " " + *type +
                      (field.mode == FieldMode::kRequired ? " NOT NULL" : ""));
  }
  return "CREATE TABLE `" + project + "." + dataset + "." + table + "`\n(\n" +
         absl::StrJoin(columns, ",\n") + "\n);";
}

std::string ViewDdl(const std::string& project, const std::string& dataset,
                    const std::string& table, const std::string& query) {
  return "CREATE VIEW `" + project + "." + dataset + "." + table + "`\nAS " + query + ";";
}

// The ID of the partition of a row of a table partitioned as `metadata` says, as a DuckDB
// expression: NULL for a table that is not partitioned.
std::string PartitionId(const TableMetadata& metadata) {
  if (metadata.time_partitioning.has_value()) {
    const std::string column = QuoteIdentifier(metadata.time_partitioning->field);
    const std::string& type = metadata.time_partitioning->type;
    const std::string format = type == "HOUR"    ? "%Y%m%d%H"
                               : type == "MONTH" ? "%Y%m"
                               : type == "YEAR"  ? "%Y"
                                                 : "%Y%m%d";
    // Partition boundaries are in UTC, which DuckDB's session time zone is.
    return "CASE WHEN " + column + " IS NULL THEN '__NULL__' WHEN CAST(" + column +
           " AS DATE) NOT BETWEEN DATE '1960-01-01' AND DATE '2159-12-31' THEN "
           "'__UNPARTITIONED__' ELSE strftime(" +
           column + ", '" + format + "') END";
  }
  if (const std::optional<RangePartitioning>& range = metadata.range_partitioning) {
    const std::string column = QuoteIdentifier(range->field);
    const std::string start = std::to_string(range->start);
    const std::string interval = std::to_string(range->interval);
    return "CASE WHEN " + column + " IS NULL THEN '__NULL__' WHEN " + column + " < " + start +
           " OR " + column + " >= " + std::to_string(range->end) +
           " THEN '__UNPARTITIONED__' ELSE CAST(" + start + " + (" + column + " - " + start +
           ") // " + interval + " * " + interval + " AS VARCHAR) END";
  }
  return "NULL";
}

// Appends a COLUMN_FIELD_PATHS row for `field` and for each field nested in it. The fields of
// a repeated record are described by their own types, as BigQuery does.
absl::Status AppendFieldPaths(const Row& identity, const std::string& column,
                              const std::string& path, const FieldSchema& field,
                              googlesql::TypeFactory* type_factory, std::vector<Row>& rows) {
  absl::StatusOr<std::string> type = TypeName(field, type_factory);
  if (!type.ok()) {
    return type.status();
  }
  rows.push_back(Concat(identity, {
                                      String(column),
                                      String(path),
                                      String(*type),
                                      OptionalString(field.description),
                                      kNull,
                                      OptionalString(field.rounding_mode),
                                      kEmptyArray,
                                  }));
  for (const FieldSchema& child : field.fields) {
    if (absl::Status status =
            AppendFieldPaths(identity, column, path + "." + child.name, child, type_factory, rows);
        !status.ok()) {
      return status;
    }
  }
  return absl::OkStatus();
}

}  // namespace

const std::vector<ViewColumn>& TablesColumns() {
  static const auto* const kColumns = new std::vector<ViewColumn>{
      {.name = "table_catalog", .type = FieldType::kString},
      {.name = "table_schema", .type = FieldType::kString},
      {.name = "table_name", .type = FieldType::kString},
      {.name = "table_type", .type = FieldType::kString},
      {.name = "managed_table_type", .type = FieldType::kString},
      {.name = "is_insertable_into", .type = FieldType::kString},
      {.name = "is_fine_grained_mutations_enabled", .type = FieldType::kString},
      {.name = "is_typed", .type = FieldType::kString},
      {.name = "is_change_history_enabled", .type = FieldType::kString},
      {.name = "creation_time", .type = FieldType::kTimestamp},
      {.name = "base_table_catalog", .type = FieldType::kString},
      {.name = "base_table_schema", .type = FieldType::kString},
      {.name = "base_table_name", .type = FieldType::kString},
      {.name = "snapshot_time_ms", .type = FieldType::kTimestamp},
      {.name = "replica_source_catalog", .type = FieldType::kString},
      {.name = "replica_source_schema", .type = FieldType::kString},
      {.name = "replica_source_name", .type = FieldType::kString},
      {.name = "replication_status", .type = FieldType::kString},
      {.name = "replication_error", .type = FieldType::kString},
      {.name = "ddl", .type = FieldType::kString},
      {.name = "default_collation_name", .type = FieldType::kString},
      {.name = "sync_status", .type = FieldType::kJson},
      {.name = "upsert_stream_apply_watermark", .type = FieldType::kTimestamp},
  };
  return *kColumns;
}

absl::StatusOr<std::vector<Row>> TablesRows(const ViewContext& context) {
  std::vector<Row> rows;
  for (const DatasetReference& dataset : context.datasets) {
    const std::string& project = dataset.project_id;
    for (const std::string& table : context.source.ListTables(project, dataset.dataset_id)) {
      const std::optional<std::vector<FieldSchema>> schema =
          context.source.FindTable(project, dataset.dataset_id, table);
      if (!schema.has_value()) {
        // Dropped since it was listed.
        continue;
      }
      std::optional<ViewDescription> view_description =
          context.source.FindView(project, dataset.dataset_id, table);
      const std::optional<std::string> view_query =
          view_description.has_value() ? std::optional(view_description->query) : std::nullopt;
      Cell ddl = kNull;
      // A clone names its base table and the time it was cloned, as snapshot_time_ms.
      std::optional<CloneDefinition> clone;
      int64_t creation_time =
          view_description.has_value() ? view_description->metadata.creation_time : 0;
      if (!view_query.has_value()) {
        if (std::optional<TableDescription> description =
                context.source.DescribeTable(project, dataset.dataset_id, table)) {
          clone = std::move(description->metadata.clone);
          creation_time = description->metadata.creation_time;
        }
        absl::StatusOr<std::string> table_ddl =
            TableDdl(project, dataset.dataset_id, table, *schema, context.type_factory);
        if (!table_ddl.ok()) {
          return table_ddl.status();
        }
        ddl = String(*table_ddl);
      } else if (!view_query->empty()) {
        ddl = String(ViewDdl(project, dataset.dataset_id, table, *view_query));
      }
      rows.push_back(
          Concat(Identity(dataset, table), {
                                               String(view_query.has_value() ? "VIEW"
                                                      : clone.has_value()    ? "CLONE"
                                                                             : "BASE TABLE"),
                                               view_query.has_value() ? kNull : String("NATIVE"),
                                               String(view_query.has_value() ? "NO" : "YES"),
                                               String("NO"),
                                               String("NO"),
                                               String("NO"),
                                               CreationTime(creation_time),
                                               clone ? String(clone->base_table.project_id) : kNull,
                                               clone ? String(clone->base_table.dataset_id) : kNull,
                                               clone ? String(clone->base_table.table_id) : kNull,
                                               clone ? String(clone->clone_time) : kNull,
                                               kNull,
                                               kNull,
                                               kNull,
                                               kNull,
                                               kNull,
                                               ddl,
                                               kNull,
                                               kNull,
                                               kNull,
                                           }));
    }
  }
  return rows;
}

const std::vector<ViewColumn>& TableOptionsColumns() {
  static const auto* const kColumns = new std::vector<ViewColumn>{
      {.name = "table_catalog", .type = FieldType::kString},
      {.name = "table_schema", .type = FieldType::kString},
      {.name = "table_name", .type = FieldType::kString},
      {.name = "option_name", .type = FieldType::kString},
      {.name = "option_type", .type = FieldType::kString},
      {.name = "option_value", .type = FieldType::kString},
  };
  return *kColumns;
}

absl::StatusOr<std::vector<Row>> TableOptionsRows(const ViewContext& context) {
  std::vector<Row> rows;
  for (const DatasetReference& dataset : context.datasets) {
    const std::string& project = dataset.project_id;
    for (const std::string& table : context.source.ListTables(project, dataset.dataset_id)) {
      std::optional<TableMetadata> metadata;
      if (std::optional<ViewDescription> view =
              context.source.FindView(project, dataset.dataset_id, table)) {
        metadata = std::move(view->metadata);
      } else if (std::optional<TableDescription> description =
                     context.source.DescribeTable(project, dataset.dataset_id, table)) {
        metadata = std::move(description->metadata);
      } else {
        continue;
      }
      std::vector<Option> options =
          CommonOptions(metadata->description, metadata->friendly_name, metadata->labels,
                        metadata->default_rounding_mode);
      if (metadata->expiration_time.has_value()) {
        options.push_back({
            .name = "expiration_timestamp",
            .type = "TIMESTAMP",
            .value = "TIMESTAMP \"" + FormatMillis(*metadata->expiration_time) + "\"",
        });
      }
      AppendOptions(Identity(dataset, table), std::move(options), rows);
    }
  }
  return rows;
}

const std::vector<ViewColumn>& PartitionsColumns() {
  static const auto* const kColumns = new std::vector<ViewColumn>{
      {.name = "table_catalog", .type = FieldType::kString},
      {.name = "table_schema", .type = FieldType::kString},
      {.name = "table_name", .type = FieldType::kString},
      {.name = "partition_id", .type = FieldType::kString},
      {.name = "total_rows", .type = FieldType::kInteger},
      {.name = "total_logical_bytes", .type = FieldType::kInteger},
      {.name = "total_billable_bytes", .type = FieldType::kInteger},
      {.name = "last_modified_time", .type = FieldType::kTimestamp},
      {.name = "storage_tier", .type = FieldType::kString},
  };
  return *kColumns;
}

// Counts the rows of each partition of each table while the query is analyzed, as the other views
// read metadata, so that a table dropped meanwhile is left out. The emulator keeps no storage
// statistics, so the byte counts are 0, as tables.get reports numBytes, and no partition has a
// last modified time.
absl::StatusOr<std::vector<Row>> PartitionsRows(const ViewContext& context) {
  std::vector<Row> rows;
  for (const DatasetReference& dataset : context.datasets) {
    const std::string& project = dataset.project_id;
    for (const std::string& table : context.source.ListTables(project, dataset.dataset_id)) {
      // Views have no partitions.
      const std::optional<TableDescription> description =
          context.source.DescribeTable(project, dataset.dataset_id, table);
      if (!description.has_value()) {
        continue;
      }
      // A table without rows has no partitions, even one that is not partitioned.
      const std::optional<std::vector<std::pair<std::optional<std::string>, int64_t>>> counts =
          context.source.CountRows(
              {.project_id = project, .dataset_id = dataset.dataset_id, .table_id = table},
              PartitionId(description->metadata));
      if (!counts.has_value()) {
        continue;
      }
      for (const auto& [partition, count] : *counts) {
        rows.push_back(
            Concat(Identity(dataset, table), {
                                                 partition.has_value() ? String(*partition) : kNull,
                                                 Int(count),
                                                 Int(0),
                                                 Int(0),
                                                 kNull,
                                                 String("ACTIVE"),
                                             }));
      }
    }
  }
  return rows;
}

const std::vector<ViewColumn>& ViewsColumns() {
  static const auto* const kColumns = new std::vector<ViewColumn>{
      {.name = "table_catalog", .type = FieldType::kString},
      {.name = "table_schema", .type = FieldType::kString},
      {.name = "table_name", .type = FieldType::kString},
      {.name = "view_definition", .type = FieldType::kString},
      {.name = "check_option", .type = FieldType::kString},
      {.name = "use_standard_sql", .type = FieldType::kString},
  };
  return *kColumns;
}

absl::StatusOr<std::vector<Row>> ViewsRows(const ViewContext& context) {
  std::vector<Row> rows;
  for (const DatasetReference& dataset : context.datasets) {
    for (const std::string& table :
         context.source.ListTables(dataset.project_id, dataset.dataset_id)) {
      const std::optional<ViewDescription> description =
          context.source.FindView(dataset.project_id, dataset.dataset_id, table);
      if (!description.has_value()) {
        continue;
      }
      rows.push_back(
          Concat(Identity(dataset, table), {
                                               OptionalString(description->query),
                                               kNull,
                                               // Views without an emulator definition have an
                                               // unknown SQL dialect too.
                                               description->query.empty() ? kNull : String("YES"),
                                           }));
    }
  }
  return rows;
}

const std::vector<ViewColumn>& ColumnsColumns() {
  static const auto* const kColumns = new std::vector<ViewColumn>{
      {.name = "table_catalog", .type = FieldType::kString},
      {.name = "table_schema", .type = FieldType::kString},
      {.name = "table_name", .type = FieldType::kString},
      {.name = "column_name", .type = FieldType::kString},
      {.name = "ordinal_position", .type = FieldType::kInteger},
      {.name = "is_nullable", .type = FieldType::kString},
      {.name = "data_type", .type = FieldType::kString},
      {.name = "is_generated", .type = FieldType::kString},
      {.name = "generation_expression", .type = FieldType::kString},
      {.name = "is_stored", .type = FieldType::kString},
      {.name = "is_hidden", .type = FieldType::kString},
      {.name = "is_updatable", .type = FieldType::kString},
      {.name = "is_system_defined", .type = FieldType::kString},
      {.name = "is_partitioning_column", .type = FieldType::kString},
      {.name = "clustering_ordinal_position", .type = FieldType::kInteger},
      {.name = "collation_name", .type = FieldType::kString},
      {.name = "column_default", .type = FieldType::kString},
      {.name = "rounding_mode", .type = FieldType::kString},
      {.name = "policy_tags", .type = FieldType::kString, /*repeated=*/.repeated = true},
      {.name = "is_identity", .type = FieldType::kString},
      {.name = "identity_generation", .type = FieldType::kString},
      {.name = "identity_start", .type = FieldType::kInteger},
      {.name = "identity_increment", .type = FieldType::kInteger},
      {.name = "identity_maximum", .type = FieldType::kInteger},
      {.name = "identity_minimum", .type = FieldType::kInteger},
      {.name = "identity_cycle", .type = FieldType::kString},
  };
  return *kColumns;
}

absl::StatusOr<std::vector<Row>> ColumnsRows(const ViewContext& context) {
  std::vector<Row> rows;
  for (const DatasetReference& dataset : context.datasets) {
    for (const std::string& table :
         context.source.ListTables(dataset.project_id, dataset.dataset_id)) {
      const std::optional<std::vector<FieldSchema>> schema =
          context.source.FindTable(dataset.project_id, dataset.dataset_id, table);
      if (!schema.has_value()) {
        continue;
      }
      for (size_t i = 0; i < schema->size(); ++i) {
        const FieldSchema& field = schema->at(i);
        absl::StatusOr<std::string> type = TypeName(field, context.type_factory);
        if (!type.ok()) {
          return type.status();
        }
        rows.push_back(Concat(Identity(dataset, table),
                              {
                                  String(field.name),
                                  Int(static_cast<int64_t>(i) + 1),
                                  String(field.mode == FieldMode::kRequired ? "NO" : "YES"),
                                  String(*type),
                                  String("NEVER"),
                                  kNull,
                                  kNull,
                                  String("NO"),
                                  kNull,
                                  String("NO"),
                                  String("NO"),
                                  kNull,
                                  kNull,
                                  OptionalString(field.default_value_expression),
                                  OptionalString(field.rounding_mode),
                                  kEmptyArray,
                                  String("NO"),
                                  kNull,
                                  kNull,
                                  kNull,
                                  kNull,
                                  kNull,
                                  kNull,
                              }));
      }
    }
  }
  return rows;
}
const std::vector<ViewColumn>& ColumnFieldPathsColumns() {
  static const auto* const kColumns = new std::vector<ViewColumn>{
      {.name = "table_catalog", .type = FieldType::kString},
      {.name = "table_schema", .type = FieldType::kString},
      {.name = "table_name", .type = FieldType::kString},
      {.name = "column_name", .type = FieldType::kString},
      {.name = "field_path", .type = FieldType::kString},
      {.name = "data_type", .type = FieldType::kString},
      {.name = "description", .type = FieldType::kString},
      {.name = "collation_name", .type = FieldType::kString},
      {.name = "rounding_mode", .type = FieldType::kString},
      {.name = "policy_tags", .type = FieldType::kString, /*repeated=*/.repeated = true},
  };
  return *kColumns;
}

absl::StatusOr<std::vector<Row>> ColumnFieldPathsRows(const ViewContext& context) {
  std::vector<Row> rows;
  for (const DatasetReference& dataset : context.datasets) {
    for (const std::string& table :
         context.source.ListTables(dataset.project_id, dataset.dataset_id)) {
      const std::optional<std::vector<FieldSchema>> schema =
          context.source.FindTable(dataset.project_id, dataset.dataset_id, table);
      if (!schema.has_value()) {
        continue;
      }
      for (const FieldSchema& field : *schema) {
        if (absl::Status status = AppendFieldPaths(Identity(dataset, table), field.name, field.name,
                                                   field, context.type_factory, rows);
            !status.ok()) {
          return status;
        }
      }
    }
  }
  return rows;
}

}  // namespace bigquery_emulator_duckdb::information_schema
