#include "src/information_schema.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

namespace bigquery_emulator_duckdb {
namespace {

// A column of a view: its BigQuery type, which is also how its DuckDB type is chosen.
struct ViewColumn {
  std::string name;
  std::string type;
  bool repeated = false;
};

// A cell as a DuckDB literal; nothing is NULL.
using Cell = std::optional<std::string>;
using Row = std::vector<Cell>;

Cell String(const std::string& value) { return QuoteLiteral(value); }
Cell Int(int64_t value) { return std::to_string(value); }
const Cell kNull;
const Cell kEmptyArray = "[]";

std::string DuckDbType(const ViewColumn& column) {
  std::string type = "VARCHAR";
  if (column.type == "INT64") {
    type = "BIGINT";
  } else if (column.type == "TIMESTAMP") {
    type = "TIMESTAMPTZ";
  } else if (column.type == "JSON") {
    type = "JSON";
  }
  return column.repeated ? type + "[]" : type;
}

const std::vector<ViewColumn>& SchemataColumns() {
  static const auto* const kColumns = new std::vector<ViewColumn>{
      {"catalog_name", "STRING"},
      {"schema_name", "STRING"},
      {"schema_owner", "STRING"},
      {"creation_time", "TIMESTAMP"},
      {"last_modified_time", "TIMESTAMP"},
      {"location", "STRING"},
      {"ddl", "STRING"},
      {"default_collation_name", "STRING"},
      {"sync_status", "JSON"},
  };
  return *kColumns;
}

const std::vector<ViewColumn>& TablesColumns() {
  static const auto* const kColumns = new std::vector<ViewColumn>{
      {"table_catalog", "STRING"},
      {"table_schema", "STRING"},
      {"table_name", "STRING"},
      {"table_type", "STRING"},
      {"managed_table_type", "STRING"},
      {"is_insertable_into", "STRING"},
      {"is_fine_grained_mutations_enabled", "STRING"},
      {"is_typed", "STRING"},
      {"is_change_history_enabled", "STRING"},
      {"creation_time", "TIMESTAMP"},
      {"base_table_catalog", "STRING"},
      {"base_table_schema", "STRING"},
      {"base_table_name", "STRING"},
      {"snapshot_time_ms", "TIMESTAMP"},
      {"replica_source_catalog", "STRING"},
      {"replica_source_schema", "STRING"},
      {"replica_source_name", "STRING"},
      {"replication_status", "STRING"},
      {"replication_error", "STRING"},
      {"ddl", "STRING"},
      {"default_collation_name", "STRING"},
      {"sync_status", "JSON"},
      {"upsert_stream_apply_watermark", "TIMESTAMP"},
  };
  return *kColumns;
}

const std::vector<ViewColumn>& ColumnsColumns() {
  static const auto* const kColumns = new std::vector<ViewColumn>{
      {"table_catalog", "STRING"},
      {"table_schema", "STRING"},
      {"table_name", "STRING"},
      {"column_name", "STRING"},
      {"ordinal_position", "INT64"},
      {"is_nullable", "STRING"},
      {"data_type", "STRING"},
      {"is_generated", "STRING"},
      {"generation_expression", "STRING"},
      {"is_stored", "STRING"},
      {"is_hidden", "STRING"},
      {"is_updatable", "STRING"},
      {"is_system_defined", "STRING"},
      {"is_partitioning_column", "STRING"},
      {"clustering_ordinal_position", "INT64"},
      {"collation_name", "STRING"},
      {"column_default", "STRING"},
      {"rounding_mode", "STRING"},
      {"policy_tags", "STRING", /*repeated=*/true},
      {"is_identity", "STRING"},
      {"identity_generation", "STRING"},
      {"identity_start", "INT64"},
      {"identity_increment", "INT64"},
      {"identity_maximum", "INT64"},
      {"identity_minimum", "INT64"},
      {"identity_cycle", "STRING"},
  };
  return *kColumns;
}

const std::vector<ViewColumn>& ColumnFieldPathsColumns() {
  static const auto* const kColumns = new std::vector<ViewColumn>{
      {"table_catalog", "STRING"}, {"table_schema", "STRING"},
      {"table_name", "STRING"},    {"column_name", "STRING"},
      {"field_path", "STRING"},    {"data_type", "STRING"},
      {"description", "STRING"},   {"collation_name", "STRING"},
      {"rounding_mode", "STRING"}, {"policy_tags", "STRING", /*repeated=*/true},
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
std::string ViewSql(const std::vector<ViewColumn>& columns, const std::vector<Row>& rows) {
  std::vector<std::string> names;
  std::vector<std::string> projections;
  for (size_t i = 0; i < columns.size(); ++i) {
    names.push_back("c" + std::to_string(i));
    projections.push_back("CAST(" + names.back() + " AS " + DuckDbType(columns[i]) + ") AS " +
                          QuoteIdentifier(columns[i].name));
  }
  std::vector<std::string> values;
  for (const Row& row : rows) {
    std::vector<std::string> cells;
    for (const Cell& cell : row) {
      cells.push_back(cell.value_or("NULL"));
    }
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
                      (field.mode == "REQUIRED" ? " NOT NULL" : ""));
  }
  return "CREATE TABLE `" + project + "." + dataset + "." + table + "`\n(\n" +
         absl::StrJoin(columns, ",\n") + "\n);";
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
  rows.push_back({String(dataset.project), String(dataset.dataset), String(table), String(column),
                  String(path), String(*type), kNull, kNull, kNull, kEmptyArray});
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
        absl::StatusOr<std::string> ddl =
            TableDdl(dataset.project, dataset.dataset, table, *schema, type_factory);
        if (!ddl.ok()) {
          return ddl.status();
        }
        rows.push_back({String(dataset.project),
                        String(dataset.dataset),
                        String(table),
                        String("BASE TABLE"),
                        String("NATIVE"),
                        String("YES"),
                        String("NO"),
                        String("NO"),
                        String("NO"),
                        kNull,
                        kNull,
                        kNull,
                        kNull,
                        kNull,
                        kNull,
                        kNull,
                        kNull,
                        kNull,
                        kNull,
                        String(*ddl),
                        kNull,
                        kNull,
                        kNull});
        continue;
      }
      for (size_t i = 0; i < schema->size(); ++i) {
        const FieldSchema& field = (*schema)[i];
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
        rows.push_back({String(dataset.project),
                        String(dataset.dataset),
                        String(table),
                        String(field.name),
                        Int(static_cast<int64_t>(i + 1)),
                        String(field.mode == "REQUIRED" ? "NO" : "YES"),
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
                        kNull});
      }
    }
  }
  return rows;
}

}  // namespace

bool IsInformationSchemaPath(const std::vector<std::string>& parts) {
  return parts.size() >= 2 && EqualsIgnoreCase(parts[parts.size() - 2], "INFORMATION_SCHEMA");
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
    if (qualifier.size() == 2 && (IsRegion(qualifier[0]) || !IsRegion(qualifier[1]))) {
      return not_found();
    }
    const std::string project =
        qualifier.empty() || IsRegion(qualifier[0]) ? default_project : qualifier[0];
    columns = SchemataColumns();
    for (const std::string& dataset : DatasetsIn(source, project, qualifier)) {
      rows.push_back({String(project), String(dataset), kNull, kNull, kNull, String("US"),
                      String(absl::StrCat("CREATE SCHEMA `", project, ".", dataset,
                                          "`\nOPTIONS(\n  location=\"us\"\n);")),
                      kNull, kNull});
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
        if (IsRegion(qualifier[0])) {
          return not_found();
        }
        project = qualifier[0];
      }
      if (!IsRegion(qualifier.back())) {
        dataset = qualifier.back();
      }
    }
    std::vector<DatasetTables> datasets;
    const std::vector<std::string> names = DatasetsIn(source, project, qualifier);
    if (dataset.has_value()) {
      if (std::find(names.begin(), names.end(), *dataset) == names.end()) {
        return not_found();
      }
      datasets.push_back({project, *dataset, source.ListTables(project, *dataset)});
    } else {
      for (const std::string& name : names) {
        datasets.push_back({project, name, source.ListTables(project, name)});
      }
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

  auto table = std::make_unique<SqlTable>(view, ViewSql(columns, rows));
  if (absl::Status status = table->set_full_name(absl::StrJoin(parts, ".")); !status.ok()) {
    return status;
  }
  for (const ViewColumn& column : columns) {
    absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(
        FieldSchema{column.name, column.type, column.repeated ? "REPEATED" : "NULLABLE", {}},
        type_factory);
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
