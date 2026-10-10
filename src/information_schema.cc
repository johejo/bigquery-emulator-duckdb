#include "src/information_schema.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/type.h"
#include "src/catalog.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/table_metadata.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {
namespace {

// A column of a view and its BigQuery type, from which its GoogleSQL and DuckDB types follow.
struct ViewColumn {
  std::string name;
  FieldType type;
  bool repeated = false;
};

FieldSchema ColumnField(const ViewColumn& column) {
  return {
      .name = column.name,
      .type = column.type,
      .mode = column.repeated ? FieldMode::kRepeated : FieldMode::kNullable,
  };
}

// A cell as a DuckDB literal; nothing is NULL.
using Cell = std::optional<std::string>;
using Row = std::vector<Cell>;

Cell String(const std::string& value) { return QuoteLiteral(value); }
Cell Int(int64_t value) { return std::to_string(value); }
const Cell kNull;
constexpr const char* kEmptyArray = "[]";

const std::vector<ViewColumn>& SchemataColumns() {
  static const auto* const kColumns = new std::vector<ViewColumn>{
      {.name = "catalog_name", .type = FieldType::kString},
      {.name = "schema_name", .type = FieldType::kString},
      {.name = "schema_owner", .type = FieldType::kString},
      {.name = "creation_time", .type = FieldType::kTimestamp},
      {.name = "last_modified_time", .type = FieldType::kTimestamp},
      {.name = "location", .type = FieldType::kString},
      {.name = "ddl", .type = FieldType::kString},
      {.name = "default_collation_name", .type = FieldType::kString},
      {.name = "sync_status", .type = FieldType::kJson},
  };
  return *kColumns;
}

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

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
  return ToUpperAscii(a) == ToUpperAscii(b);
}

bool IsRegion(const std::string& part) { return ToLowerAscii(part).starts_with("region-"); }

// The datasets of `project` in the region the qualifier names, if it names one. Every dataset
// is in the US, as the emulator reports for all of them.
std::vector<std::string> DatasetsIn(TableSource& source, const std::string& project,
                                    const std::vector<std::string>& qualifier) {
  if (!qualifier.empty() && IsRegion(qualifier.back()) &&
      ToLowerAscii(qualifier.back()) != "region-us") {
    return {};
  }
  return source.ListDatasets(project);
}

// The tables of one dataset, as a view scoped to a dataset or a region lists them.
struct DatasetTables {
  std::string project;
  std::string dataset;
  std::vector<std::string> tables;
};

// Selects the rows as `columns`. The values are cast in one outer SELECT so that a column is
// typed even when every row has NULL in it.
absl::StatusOr<std::string> ViewSql(const std::vector<ViewColumn>& columns,
                                    const std::vector<Row>& rows) {
  std::vector<std::string> names;
  std::vector<std::string> projections;
  for (size_t i = 0; i < columns.size(); ++i) {
    absl::StatusOr<std::string> type = DuckDbColumnType(ColumnField(columns.at(i)));
    if (!type.ok()) {
      return type.status();
    }
    names.push_back("c" + std::to_string(i));
    projections.push_back("CAST(" + names.back() + " AS " + *type + ") AS " +
                          QuoteIdentifier(columns.at(i).name));
  }
  std::vector<std::string> values;
  for (const Row& row : rows) {
    std::vector<std::string> cells;
    std::ranges::transform(row, std::back_inserter(cells),
                           [](const Cell& cell) { return cell.value_or("NULL"); });
    values.push_back("(" + absl::StrJoin(cells, ", ") + ")");
  }
  if (rows.empty()) {
    values.push_back("(" + absl::StrJoin(std::vector<std::string>(columns.size(), "NULL"), ", ") +
                     ")");
  }
  return "SELECT " + absl::StrJoin(projections, ", ") + " FROM (VALUES " +
         absl::StrJoin(values, ", ") + ") AS v(" + absl::StrJoin(names, ", ") + ")" +
         (rows.empty() ? " WHERE false" : "");
}

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

// Appends a COLUMN_FIELD_PATHS row for `field` and for each field nested in it. The fields of
// a repeated record are described by their own types, as BigQuery does.
absl::Status AppendFieldPaths(const DatasetTables& dataset, const std::string& table,
                              const std::string& column, const std::string& path,
                              const FieldSchema& field, googlesql::TypeFactory* type_factory,
                              std::vector<Row>& rows) {
  absl::StatusOr<std::string> type = TypeName(field, type_factory);
  if (!type.ok()) {
    return type.status();
  }
  rows.push_back({
      String(dataset.project),
      String(dataset.dataset),
      String(table),
      String(column),
      String(path),
      String(*type),
      kNull,
      kNull,
      kNull,
      kEmptyArray,
  });
  for (const FieldSchema& child : field.fields) {
    if (absl::Status status = AppendFieldPaths(dataset, table, column, path + "." + child.name,
                                               child, type_factory, rows);
        !status.ok()) {
      return status;
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<std::vector<Row>> TableRows(TableSource& source,
                                           const std::vector<DatasetTables>& datasets,
                                           const std::string& view,
                                           googlesql::TypeFactory* type_factory) {
  std::vector<Row> rows;
  for (const DatasetTables& dataset : datasets) {
    for (const std::string& table : dataset.tables) {
      const std::optional<std::vector<FieldSchema>> schema =
          source.FindTable(dataset.project, dataset.dataset, table);
      if (!schema.has_value()) {
        // Dropped since it was listed.
        continue;
      }
      if (view == "TABLES") {
        const std::optional<std::string> view_query =
            source.FindViewQuery(dataset.project, dataset.dataset, table);
        Cell ddl = kNull;
        // A clone names its base table and the time it was cloned, as snapshot_time_ms.
        std::optional<CloneDefinition> clone;
        if (!view_query.has_value()) {
          if (std::optional<TableDescription> description =
                  source.DescribeTable(dataset.project, dataset.dataset, table)) {
            clone = std::move(description->metadata.clone);
          }
          absl::StatusOr<std::string> table_ddl =
              TableDdl(dataset.project, dataset.dataset, table, *schema, type_factory);
          if (!table_ddl.ok()) {
            return table_ddl.status();
          }
          ddl = String(*table_ddl);
        } else if (!view_query->empty()) {
          ddl = String(ViewDdl(dataset.project, dataset.dataset, table, *view_query));
        }
        rows.push_back({
            String(dataset.project),
            String(dataset.dataset),
            String(table),
            String(view_query.has_value() ? "VIEW"
                   : clone.has_value()    ? "CLONE"
                                          : "BASE TABLE"),
            view_query.has_value() ? kNull : String("NATIVE"),
            String(view_query.has_value() ? "NO" : "YES"),
            String("NO"),
            String("NO"),
            String("NO"),
            kNull,
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
        });
        continue;
      }
      for (size_t i = 0; i < schema->size(); ++i) {
        const FieldSchema& field = (*schema).at(i);
        if (view == "COLUMN_FIELD_PATHS") {
          if (absl::Status status = AppendFieldPaths(dataset, table, field.name, field.name, field,
                                                     type_factory, rows);
              !status.ok()) {
            return status;
          }
          continue;
        }
        absl::StatusOr<std::string> type = TypeName(field, type_factory);
        if (!type.ok()) {
          return type.status();
        }
        rows.push_back({
            String(dataset.project),
            String(dataset.dataset),
            String(table),
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
            kNull,
            kNull,
            kEmptyArray,
            String("NO"),
            kNull,
            kNull,
            kNull,
            kNull,
            kNull,
            kNull,
        });
      }
    }
  }
  return rows;
}

}  // namespace

bool IsInformationSchemaPath(const std::vector<std::string>& parts) {
  return parts.size() >= 2 && EqualsIgnoreCase(parts.at(parts.size() - 2), "INFORMATION_SCHEMA");
}

// The order of the defaults follows BigQueryCatalog's.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
absl::StatusOr<std::unique_ptr<SqlTable>> InformationSchemaView(
    const std::vector<std::string>& parts, TableSource& source,
    googlesql::TypeFactory* type_factory, const std::string& default_project,
    const std::string& default_dataset) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  const std::string view = ToUpperAscii(parts.back());
  const std::vector<std::string> qualifier(parts.begin(), parts.end() - 2);
  const auto not_found = [&parts] {
    return absl::NotFoundError("Table not found: " + absl::StrJoin(parts, "."));
  };
  if (qualifier.size() > 2) {
    return not_found();
  }

  std::vector<ViewColumn> columns;
  std::vector<Row> rows;
  if (view == "SCHEMATA") {
    // Scoped to a project, optionally with a region: [PROJECT.][`region-REGION`.]
    if (qualifier.size() == 2 && (IsRegion(qualifier.at(0)) || !IsRegion(qualifier.at(1)))) {
      return not_found();
    }
    const std::string project =
        qualifier.empty() || IsRegion(qualifier.at(0)) ? default_project : qualifier.at(0);
    columns = SchemataColumns();
    for (const std::string& dataset : DatasetsIn(source, project, qualifier)) {
      // The row layout reads directly as the information schema's column order.
      // cppcheck-suppress useStlAlgorithm
      rows.push_back({
          String(project),
          String(dataset),
          kNull,
          kNull,
          kNull,
          String("US"),
          String(absl::StrCat("CREATE SCHEMA `", project, ".", dataset,
                              "`\nOPTIONS(\n  location=\"us\"\n);")),
          kNull,
          kNull,
      });
    }
  } else if (view == "TABLES" || view == "COLUMNS" || view == "COLUMN_FIELD_PATHS") {
    // Scoped to a dataset or to a region: [PROJECT.](DATASET | `region-REGION`).
    std::string project = default_project;
    std::optional<std::string> dataset;
    if (qualifier.empty()) {
      if (default_dataset.empty()) {
        return absl::InvalidArgumentError("Table \"" + absl::StrJoin(parts, ".") +
                                          "\" must be qualified with a dataset (e.g. "
                                          "dataset.table).");
      }
      dataset = default_dataset;
    } else {
      if (qualifier.size() == 2) {
        if (IsRegion(qualifier.at(0))) {
          return not_found();
        }
        project = qualifier.at(0);
      }
      if (!IsRegion(qualifier.back())) {
        dataset = qualifier.back();
      }
    }
    std::vector<DatasetTables> datasets;
    const std::vector<std::string> names = DatasetsIn(source, project, qualifier);
    if (dataset.has_value()) {
      if (std::ranges::find(names, *dataset) == names.end()) {
        return not_found();
      }
      datasets.push_back({
          .project = project,
          .dataset = *dataset,
          .tables = source.ListTables(project, *dataset),
      });
    } else {
      std::ranges::transform(names, std::back_inserter(datasets), [&](const std::string& name) {
        return DatasetTables{
            .project = project,
            .dataset = name,
            .tables = source.ListTables(project, name),
        };
      });
    }
    columns = view == "TABLES"    ? TablesColumns()
              : view == "COLUMNS" ? ColumnsColumns()
                                  : ColumnFieldPathsColumns();
    absl::StatusOr<std::vector<Row>> table_rows = TableRows(source, datasets, view, type_factory);
    if (!table_rows.ok()) {
      return table_rows.status();
    }
    rows = *std::move(table_rows);
  } else {
    return not_found();
  }

  absl::StatusOr<std::string> sql = ViewSql(columns, rows);
  if (!sql.ok()) {
    return sql.status();
  }
  auto table = std::make_unique<SqlTable>(view, *std::move(sql));
  if (absl::Status status = table->set_full_name(absl::StrJoin(parts, ".")); !status.ok()) {
    return status;
  }
  for (const ViewColumn& column : columns) {
    absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(ColumnField(column), type_factory);
    if (!type.ok()) {
      return type.status();
    }
    if (absl::Status status =
            table->AddColumn(std::make_unique<googlesql::SimpleColumn>(view, column.name, *type));
        !status.ok()) {
      return status;
    }
  }
  return table;
}

}  // namespace bigquery_emulator_duckdb
