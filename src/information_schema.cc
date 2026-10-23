#include "src/information_schema.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_join.h"
#include "googlesql/public/simple_catalog.h"
#include "src/catalog_table.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/information_schema_datasets.h"
#include "src/information_schema_routines.h"
#include "src/information_schema_rows.h"
#include "src/information_schema_tables.h"
#include "src/information_schema_view.h"
#include "src/location.h"
#include "src/references.h"
#include "src/table_source.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {
namespace {

namespace info = information_schema;
using info::Cell;
using info::Row;
using info::ViewColumn;
using info::ViewContext;

FieldSchema ColumnField(const ViewColumn& column) {
  return {
      .name = std::string(column.name),
      .type = column.type,
      .mode = column.repeated ? FieldMode::kRepeated : FieldMode::kNullable,
  };
}

// Selects the rows as `columns`. The values are cast in one outer SELECT so that a column is
// typed even when every row has NULL in it.
absl::StatusOr<std::string> ViewSql(std::span<const ViewColumn> columns,
                                    const std::vector<Row>& rows) {
  std::vector<std::string> names;
  std::vector<std::string> projections;
  for (const ViewColumn& column : columns) {
    absl::StatusOr<std::string> type = DuckDbColumnType(ColumnField(column));
    if (!type.ok()) {
      return type.status();
    }
    names.push_back("c" + std::to_string(names.size()));
    projections.push_back("CAST(" + names.back() + " AS " + *type + ") AS " +
                          QuoteIdentifier(column.name));
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

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
  return ToUpperAscii(a) == ToUpperAscii(b);
}

// Whether `project` is spelled as BigQuery requires of a project ID: 6 to 63 lowercase letters,
// digits or dashes, starting with a letter and not ending with a dash, after an optional domain.
bool ValidProjectId(const std::string& project) {
  const size_t colon = project.rfind(':');
  const std::string id = colon == std::string::npos ? project : project.substr(colon + 1);
  const auto allowed = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
  };
  return id.size() >= 6 && id.size() <= 63 && id.front() >= 'a' && id.front() <= 'z' &&
         id.back() != '-' && std::ranges::all_of(id, allowed);
}

// The error BigQuery gives for a project qualifier that is no project ID, such as `REGION-EU`.
// The emulator's own projects, which need not be spelled so, are left alone.
std::optional<absl::Status> InvalidProject(TableSource& source, const std::string& project) {
  if (ValidProjectId(project) || source.HasProject(project)) {
    return std::nullopt;
  }
  return absl::InvalidArgumentError(
      "Invalid project ID '" + project +
      "'. Project IDs must contain 6-63 lowercase letters, digits, or dashes. Some project IDs "
      "also include domain name separated by a colon. IDs must start with a letter and may not "
      "end with a dash.");
}

// BigQuery reads `REGION-EU` as a project, not a region.
bool IsRegion(const std::string& part) { return part.starts_with(kRegionPrefix); }

// The datasets of `project` in the region the qualifier names, or in the US for a view that names
// none, as BigQuery documents for SCHEMATA; the job must run there. An error for a region BigQuery
// does not have.
absl::StatusOr<std::vector<DatasetReference>> DatasetsIn(
    TableSource& source, const std::string& project, const std::vector<std::string>& qualifier) {
  std::string location = kDefaultLocation;
  std::string region_name = "region-us";
  if (!qualifier.empty() && IsRegion(qualifier.back())) {
    region_name = qualifier.back();
    std::optional<std::string> region = RegionQualifierLocation(region_name);
    if (!region.has_value()) {
      return absl::InvalidArgumentError("Unrecognized region: " +
                                        region_name.substr(kRegionPrefix.size()));
    }
    location = *std::move(region);
  }
  // BigQuery names a region of another location as a dataset of the project.
  source.ReferenceLocation("Dataset " + project + ":" + region_name, location);
  std::vector<DatasetReference> datasets;
  for (std::string& dataset : source.ListDatasets(project)) {
    if (source.DatasetLocation(project, dataset) == location) {
      datasets.push_back({.project_id = project, .dataset_id = std::move(dataset)});
    }
  }
  return datasets;
}

// Which qualifiers name the resources a view describes.
enum class ViewScope : std::uint8_t {
  // [PROJECT.][`region-REGION`.]VIEW: the datasets of a project in a region, the US by default.
  kProject,
  // [PROJECT.](DATASET | `region-REGION`).VIEW, or the default dataset.
  kDatasetOrRegion,
  // [PROJECT.]DATASET.VIEW, or the default dataset.
  kDataset,
};

// An INFORMATION_SCHEMA view: its scope, its columns and the function that computes its rows.
struct ViewDefinition {
  std::string_view name;
  ViewScope scope;
  std::span<const ViewColumn> columns;
  absl::StatusOr<std::vector<Row>> (*rows)(const ViewContext&);
};

constexpr std::array kViews = {
    ViewDefinition{
        .name = "SCHEMATA",
        .scope = ViewScope::kProject,
        .columns = info::kSchemataColumns,
        .rows = info::SchemataRows,
    },
    ViewDefinition{
        .name = "SCHEMATA_OPTIONS",
        .scope = ViewScope::kProject,
        .columns = info::kSchemataOptionsColumns,
        .rows = info::SchemataOptionsRows,
    },
    ViewDefinition{
        .name = "TABLES",
        .scope = ViewScope::kDatasetOrRegion,
        .columns = info::kTablesColumns,
        .rows = info::TablesRows,
    },
    ViewDefinition{
        .name = "TABLE_SNAPSHOTS",
        .scope = ViewScope::kDatasetOrRegion,
        .columns = info::kTableSnapshotsColumns,
        .rows = info::TableSnapshotsRows,
    },
    ViewDefinition{
        .name = "TABLE_OPTIONS",
        .scope = ViewScope::kDatasetOrRegion,
        .columns = info::kTableOptionsColumns,
        .rows = info::TableOptionsRows,
    },
    ViewDefinition{
        .name = "PARTITIONS",
        .scope = ViewScope::kDataset,
        .columns = info::kPartitionsColumns,
        .rows = info::PartitionsRows,
    },
    ViewDefinition{
        .name = "VIEWS",
        .scope = ViewScope::kDatasetOrRegion,
        .columns = info::kViewsColumns,
        .rows = info::ViewsRows,
    },
    ViewDefinition{
        .name = "COLUMNS",
        .scope = ViewScope::kDatasetOrRegion,
        .columns = info::kColumnsColumns,
        .rows = info::ColumnsRows,
    },
    ViewDefinition{
        .name = "COLUMN_FIELD_PATHS",
        .scope = ViewScope::kDatasetOrRegion,
        .columns = info::kColumnFieldPathsColumns,
        .rows = info::ColumnFieldPathsRows,
    },
    ViewDefinition{
        .name = "ROUTINES",
        .scope = ViewScope::kDatasetOrRegion,
        .columns = info::kRoutinesColumns,
        .rows = info::RoutinesRows,
    },
    ViewDefinition{
        .name = "PARAMETERS",
        .scope = ViewScope::kDatasetOrRegion,
        .columns = info::kParametersColumns,
        .rows = info::ParametersRows,
    },
};

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
  const auto* const definition =
      std::ranges::find(kViews, std::string_view(view), &ViewDefinition::name);
  if (qualifier.size() > 2 || definition == kViews.end()) {
    return not_found();
  }

  ViewContext context{.source = source, .type_factory = type_factory, .datasets = {}};
  if (definition->scope == ViewScope::kProject) {
    if (qualifier.size() == 2 && (IsRegion(qualifier.at(0)) || !IsRegion(qualifier.at(1)))) {
      return not_found();
    }
    const std::string project = qualifier.empty() || IsRegion(qualifier.at(0))
                                    ? default_project
                                    : source.ProjectId(qualifier.at(0));
    if (std::optional<absl::Status> invalid = InvalidProject(source, project)) {
      return *std::move(invalid);
    }
    absl::StatusOr<std::vector<DatasetReference>> datasets = DatasetsIn(source, project, qualifier);
    if (!datasets.ok()) {
      return datasets.status();
    }
    context.datasets = *std::move(datasets);
  } else {
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
        project = source.ProjectId(qualifier.at(0));
        if (std::optional<absl::Status> invalid = InvalidProject(source, project)) {
          return *std::move(invalid);
        }
      }
      if (!IsRegion(qualifier.back())) {
        dataset = qualifier.back();
      } else if (definition->scope == ViewScope::kDataset) {
        // BigQuery's answer for a region qualifier on a view it has only for datasets.
        const std::string table =
            project + ":" +
            absl::StrJoin(parts.begin() + (qualifier.size() == 2 ? 1 : 0), parts.end(), ".");
        return absl::PermissionDeniedError("Access Denied: Table " + table +
                                           ": User does not have permission to query table " +
                                           table + ", or perhaps it does not exist.");
      }
    }
    if (dataset.has_value()) {
      const std::optional<std::string> location = source.DatasetLocation(project, *dataset);
      if (!location.has_value()) {
        return not_found();
      }
      source.ReferenceLocation("Dataset " + project + ":" + *dataset, *location);
      context.datasets.push_back({.project_id = project, .dataset_id = *dataset});
    } else {
      absl::StatusOr<std::vector<DatasetReference>> datasets =
          DatasetsIn(source, project, qualifier);
      if (!datasets.ok()) {
        return datasets.status();
      }
      context.datasets = *std::move(datasets);
    }
  }

  const std::span<const ViewColumn> columns = definition->columns;
  absl::StatusOr<std::vector<Row>> rows = definition->rows(context);
  if (!rows.ok()) {
    return rows.status();
  }
  absl::StatusOr<std::string> sql = ViewSql(columns, *rows);
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
