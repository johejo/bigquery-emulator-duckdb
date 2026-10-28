#include "src/information_schema_tables.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_join.h"
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

// DDL omits redundant collations: the table's default, and binary when there is no nonempty
// default. Nested STRING fields and array elements carry their own COLLATE clauses.
absl::StatusOr<std::string> ColumnTypeDdl(FieldSchema field,
                                          const std::optional<std::string>& default_collation,
                                          googlesql::TypeFactory* type_factory) {
  const bool repeated = field.mode == FieldMode::kRepeated;
  field.mode = FieldMode::kNullable;
  std::string text;
  if (field.type == FieldType::kRecord) {
    std::vector<std::string> children;
    for (const FieldSchema& child : field.fields) {
      auto type = ColumnTypeDdl(child, default_collation, type_factory);
      if (!type.ok()) {
        return type.status();
      }
      children.push_back(child.name + " " + *type);
    }
    text = "STRUCT<" + absl::StrJoin(children, ", ") + ">";
  } else {
    auto type = GoogleSqlTypeName(field, type_factory);
    if (!type.ok()) {
      return type.status();
    }
    text = *type;
    if (field.collation.has_value() && *field.collation != default_collation.value_or("")) {
      text += " COLLATE " + QuoteLiteral(*field.collation);
    }
  }
  return repeated ? "ARRAY<" + text + ">" : text;
}

absl::StatusOr<std::string> TableDdl(const std::string& project, const std::string& dataset,
                                     const std::string& table,
                                     const std::vector<FieldSchema>& schema,
                                     const std::optional<std::string>& default_collation,
                                     const std::optional<GcsExternalData>& external,
                                     googlesql::TypeFactory* type_factory) {
  std::vector<std::string> columns;
  for (const FieldSchema& field : schema) {
    absl::StatusOr<std::string> type = ColumnTypeDdl(field, default_collation, type_factory);
    if (!type.ok()) {
      return type.status();
    }
    columns.push_back("  " + field.name + " " + *type +
                      (field.mode == FieldMode::kRequired ? " NOT NULL" : ""));
  }
  std::string ddl = std::string(external ? "CREATE EXTERNAL TABLE `" : "CREATE TABLE `") + project +
                    "." + dataset + "." + table + "`";
  if (!external) {
    ddl += "\n(\n" + absl::StrJoin(columns, ",\n") + "\n)";
  }
  if (external) {
    std::vector<std::string> uris;
    std::ranges::transform(external->uris, std::back_inserter(uris), StringOptionValue);
    ddl += "\nOPTIONS(\n  format=\"PARQUET\",\n  uris=[" + absl::StrJoin(uris, ", ") + "]\n)";
  }
  return ddl + ";";
}

std::string ViewDdl(const std::string& project, const std::string& dataset,
                    const std::string& table, const std::string& query, bool materialized) {
  return std::string(materialized ? "CREATE MATERIALIZED VIEW `" : "CREATE VIEW `") + project +
         "." + dataset + "." + table + "`\nAS " + query + ";";
}

// The ID of the partition of a row of a table partitioned as `metadata` says, as a DuckDB
// expression: NULL for a table that is not partitioned.
std::string PartitionId(const TableMetadata& metadata) {
  if (metadata.time_partitioning.has_value()) {
    const std::string column = QuoteIdentifier(metadata.time_partitioning->field);
    std::string format;
    switch (metadata.time_partitioning->type) {
      case TimePartitionUnit::kDay:
        format = "%Y%m%d";
        break;
      case TimePartitionUnit::kHour:
        format = "%Y%m%d%H";
        break;
      case TimePartitionUnit::kMonth:
        format = "%Y%m";
        break;
      case TimePartitionUnit::kYear:
        format = "%Y";
        break;
    }
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
  return "CAST(NULL AS VARCHAR)";
}

// Appends a COLUMN_FIELD_PATHS row for `field` and for each field nested in it. The fields of
// a repeated record are described by their own types, as BigQuery does.
absl::Status AppendFieldPaths(const Row& identity, const std::string& column,
                              const std::string& path, const FieldSchema& field,
                              googlesql::TypeFactory* type_factory, std::vector<Row>& rows) {
  absl::StatusOr<std::string> type = GoogleSqlTypeName(field, type_factory);
  if (!type.ok()) {
    return type.status();
  }
  rows.push_back(
      Concat(identity, {
                           String(column),
                           String(path),
                           String(*type),
                           OptionalString(field.description),
                           // BigQuery spells a field without collation "NULL".
                           // BigQuery spells a column without collation "NULL".
                           String(field.collation.value_or("NULL")),
                           OptionalString(std::string(RoundingModeName(field.rounding_mode))),
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
      // A clone or snapshot names its base table and the time it was made, as snapshot_time_ms.
      std::optional<BaseTableDefinition> base;
      bool snapshot = false;
      std::optional<GcsExternalData> external;
      std::optional<std::string> default_collation;
      int64_t creation_time =
          view_description.has_value() ? view_description->metadata.creation_time : 0;
      if (!view_query.has_value()) {
        if (std::optional<TableDescription> description =
                context.source.DescribeTable(project, dataset.dataset_id, table)) {
          snapshot = description->metadata.snapshot.has_value();
          external = description->metadata.external_data;
          base = snapshot ? std::move(description->metadata.snapshot)
                          : std::move(description->metadata.clone);
          creation_time = description->metadata.creation_time;
          default_collation = description->metadata.default_collation;
        }
        absl::StatusOr<std::string> table_ddl =
            TableDdl(project, dataset.dataset_id, table, *schema, default_collation, external,
                     context.type_factory);
        if (!table_ddl.ok()) {
          return table_ddl.status();
        }
        if (default_collation.has_value()) {
          table_ddl->pop_back();
          *table_ddl += "\nDEFAULT COLLATE " + QuoteLiteral(*default_collation) + ";";
        }
        ddl = String(*table_ddl);
      } else if (!view_query->empty()) {
        ddl = String(ViewDdl(project, dataset.dataset_id, table, *view_query,
                             view_description->materialized));
      }
      rows.push_back(
          Concat(Identity(dataset, table),
                 {
                     String(view_description && view_description->materialized ? "MATERIALIZED VIEW"
                            : view_query.has_value()                           ? "VIEW"
                            : external                                         ? "EXTERNAL"
                            : snapshot                                         ? "SNAPSHOT"
                            : base.has_value()                                 ? "CLONE"
                                                                               : "BASE TABLE"),
                     view_query.has_value() || external ? kNull : String("NATIVE"),
                     String(view_query.has_value() || snapshot || external ? "NO" : "YES"),
                     String("NO"),
                     String("NO"),
                     String("NO"),
                     CreationTime(creation_time),
                     base ? String(base->base_table.project_id) : kNull,
                     base ? String(base->base_table.dataset_id) : kNull,
                     base ? String(base->base_table.table_id) : kNull,
                     base ? String(base->time) : kNull,
                     kNull,
                     kNull,
                     kNull,
                     kNull,
                     kNull,
                     ddl,
                     default_collation.has_value() ? String(*default_collation) : kNull,
                     kNull,
                     kNull,
                 }));
    }
  }
  return rows;
}

absl::StatusOr<std::vector<Row>> TableSnapshotsRows(const ViewContext& context) {
  std::vector<Row> rows;
  for (const DatasetReference& dataset : context.datasets) {
    for (const std::string& table :
         context.source.ListTables(dataset.project_id, dataset.dataset_id)) {
      const std::optional<TableDescription> description =
          context.source.DescribeTable(dataset.project_id, dataset.dataset_id, table);
      if (!description.has_value() || !description->metadata.snapshot.has_value()) {
        continue;
      }
      const BaseTableDefinition& snapshot = *description->metadata.snapshot;
      rows.push_back(Concat(Identity(dataset, table), {
                                                          String(snapshot.base_table.project_id),
                                                          String(snapshot.base_table.dataset_id),
                                                          String(snapshot.base_table.table_id),
                                                          String(snapshot.time),
                                                      }));
    }
  }
  return rows;
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
                        metadata->default_rounding_mode, metadata->label_order,
                        metadata->resource_tags, metadata->tag_order);
      if (metadata->external_data) {
        std::vector<std::string> uris;
        std::ranges::transform(metadata->external_data->uris, std::back_inserter(uris),
                               StringOptionValue);
        options.push_back({.name = "format", .type = "STRING", .value = "\"PARQUET\""});
        options.push_back({
            .name = "uris",
            .type = "ARRAY<STRING>",
            .value = "[" + absl::StrJoin(uris, ", ") + "]",
        });
      }
      if (metadata->restored_from_snapshot) {
        if (metadata->friendly_name.empty()) {
          options.push_back({.name = "friendly_name", .type = "STRING", .value = "\"\""});
        }
        if (metadata->labels.empty()) {
          options.push_back(
              {.name = "labels", .type = "ARRAY<STRUCT<STRING, STRING>>", .value = "[]"});
        }
      }
      if (metadata->require_partition_filter) {
        options.push_back({.name = "require_partition_filter", .type = "BOOL", .value = "true"});
      }
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

absl::StatusOr<std::vector<Row>> ViewsRows(const ViewContext& context) {
  std::vector<Row> rows;
  for (const DatasetReference& dataset : context.datasets) {
    for (const std::string& table :
         context.source.ListTables(dataset.project_id, dataset.dataset_id)) {
      const std::optional<ViewDescription> description =
          context.source.FindView(dataset.project_id, dataset.dataset_id, table);
      if (!description.has_value() || description->materialized) {
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
        absl::StatusOr<std::string> type = GoogleSqlTypeName(field, context.type_factory);
        if (!type.ok()) {
          return type.status();
        }
        rows.push_back(
            Concat(Identity(dataset, table),
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
                       String(field.collation.value_or("NULL")),
                       OptionalString(field.default_value_expression),
                       OptionalString(std::string(RoundingModeName(field.rounding_mode))),
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
