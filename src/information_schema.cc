#include "src/information_schema.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/time/time.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/strings.h"
#include "googlesql/public/type.h"
#include "src/catalog.h"
#include "src/catalog_table.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/location.h"
#include "src/references.h"
#include "src/routine.h"
#include "src/table_metadata.h"
#include "src/table_source.h"
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

// A time in milliseconds since the Unix epoch as INFORMATION_SCHEMA spells it.
std::string FormatMillis(int64_t millis) {
  return absl::FormatTime("%Y-%m-%dT%H:%M:%E3SZ", absl::FromUnixMillis(millis),
                          absl::UTCTimeZone());
}

// The time a dataset, table or view was created, or NULL for one created before the emulator
// recorded it.
Cell CreationTime(int64_t millis) {
  if (millis == 0) {
    return kNull;
  }
  return String(FormatMillis(millis));
}

// An unset description, default or rounding mode, which the table resource omits, is NULL.
Cell OptionalString(const std::string& value) { return value.empty() ? kNull : String(value); }

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

const std::vector<ViewColumn>& SchemataOptionsColumns() {
  static const auto* const kColumns = new std::vector<ViewColumn>{
      {.name = "catalog_name", .type = FieldType::kString},
      {.name = "schema_name", .type = FieldType::kString},
      {.name = "option_name", .type = FieldType::kString},
      {.name = "option_type", .type = FieldType::kString},
      {.name = "option_value", .type = FieldType::kString},
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

const std::vector<ViewColumn>& RoutinesColumns() {
  static const auto* const kColumns = new std::vector<ViewColumn>{
      {.name = "specific_catalog", .type = FieldType::kString},
      {.name = "specific_schema", .type = FieldType::kString},
      {.name = "specific_name", .type = FieldType::kString},
      {.name = "routine_catalog", .type = FieldType::kString},
      {.name = "routine_schema", .type = FieldType::kString},
      {.name = "routine_name", .type = FieldType::kString},
      {.name = "routine_type", .type = FieldType::kString},
      {.name = "data_type", .type = FieldType::kString},
      {.name = "routine_body", .type = FieldType::kString},
      {.name = "routine_definition", .type = FieldType::kString},
      {.name = "external_language", .type = FieldType::kString},
      {.name = "is_deterministic", .type = FieldType::kString},
      {.name = "security_type", .type = FieldType::kString},
      {.name = "created", .type = FieldType::kTimestamp},
      {.name = "last_altered", .type = FieldType::kTimestamp},
      {.name = "ddl", .type = FieldType::kString},
      {.name = "connection", .type = FieldType::kString},
  };
  return *kColumns;
}

const std::vector<ViewColumn>& ParametersColumns() {
  static const auto* const kColumns = new std::vector<ViewColumn>{
      {.name = "specific_catalog", .type = FieldType::kString},
      {.name = "specific_schema", .type = FieldType::kString},
      {.name = "specific_name", .type = FieldType::kString},
      {.name = "ordinal_position", .type = FieldType::kInteger},
      {.name = "parameter_mode", .type = FieldType::kString},
      {.name = "is_result", .type = FieldType::kString},
      {.name = "parameter_name", .type = FieldType::kString},
      {.name = "data_type", .type = FieldType::kString},
      {.name = "parameter_default", .type = FieldType::kString},
      {.name = "is_aggregate", .type = FieldType::kString},
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

// What a view describes: the datasets its qualifier selects.
struct ViewContext {
  TableSource& source;
  googlesql::TypeFactory* type_factory;
  std::vector<DatasetReference> datasets;
};

// The rows of a view: rows given cell by cell, and DuckDB queries that compute more, each
// selecting the view's columns in order.
struct ViewRows {
  std::vector<Row> rows;
  std::vector<std::string> queries;
};

// Selects the rows as `columns`. The values are cast in one outer SELECT so that a column is
// typed even when every row has NULL in it.
absl::StatusOr<std::string> ViewSql(const std::vector<ViewColumn>& columns, const ViewRows& rows) {
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
  std::vector<std::string> sources = rows.queries;
  if (!rows.rows.empty()) {
    std::vector<std::string> values;
    for (const Row& row : rows.rows) {
      std::vector<std::string> cells;
      std::ranges::transform(row, std::back_inserter(cells),
                             [](const Cell& cell) { return cell.value_or("NULL"); });
      values.push_back("(" + absl::StrJoin(cells, ", ") + ")");
    }
    sources.push_back("SELECT * FROM (VALUES " + absl::StrJoin(values, ", ") + ")");
  }
  const bool empty = sources.empty();
  if (empty) {
    sources.push_back("SELECT " +
                      absl::StrJoin(std::vector<std::string>(columns.size(), "NULL"), ", "));
  }
  return "SELECT " + absl::StrJoin(projections, ", ") + " FROM (" +
         absl::StrJoin(sources, " UNION ALL ") + ") AS v(" + absl::StrJoin(names, ", ") + ")" +
         (empty ? " WHERE false" : "");
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

// The cells that name a dataset or a table in a row: its project, dataset and, for a table, its
// name.
Row Identity(const DatasetReference& dataset) {
  return {String(dataset.project_id), String(dataset.dataset_id)};
}

Row Identity(const DatasetReference& dataset, const std::string& table) {
  return {String(dataset.project_id), String(dataset.dataset_id), String(table)};
}

// `identity` followed by `cells`.
Row Concat(Row identity, const Row& cells) {
  identity.insert(identity.end(), cells.begin(), cells.end());
  return identity;
}

// An option as SCHEMATA_OPTIONS and TABLE_OPTIONS list it: its name, its type and its value as
// a GoogleSQL literal.
struct Option {
  std::string name;
  std::string type;
  std::string value;
};

std::string StringOptionValue(const std::string& value) {
  return googlesql::ToDoubleQuotedStringLiteral(value);
}

// A FLOAT64 option value in its shortest round-trip form, with a fraction as BigQuery shows it,
// such as 90.0.
std::string Float64OptionValue(double value) {
  std::array<char, 32> buffer{};
  const std::to_chars_result result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  std::string text(buffer.data(), result.ptr);
  if (text.find_first_of(".e") == std::string::npos) {
    text += ".0";
  }
  return text;
}

// The options that metadata of a dataset, a table or a view sets, which are common to both.
std::vector<Option> CommonOptions(const std::string& description, const std::string& friendly_name,
                                  const std::map<std::string, std::string>& labels,
                                  const std::string& default_rounding_mode) {
  std::vector<Option> options;
  if (!default_rounding_mode.empty()) {
    options.push_back({
        .name = "default_rounding_mode",
        .type = "STRING",
        .value = StringOptionValue(default_rounding_mode),
    });
  }
  if (!description.empty()) {
    options.push_back(
        {.name = "description", .type = "STRING", .value = StringOptionValue(description)});
  }
  if (!friendly_name.empty()) {
    options.push_back(
        {.name = "friendly_name", .type = "STRING", .value = StringOptionValue(friendly_name)});
  }
  if (!labels.empty()) {
    std::vector<std::string> entries;
    std::ranges::transform(labels, std::back_inserter(entries), [](const auto& label) {
      return "STRUCT(" + StringOptionValue(label.first) + ", " + StringOptionValue(label.second) +
             ")";
    });
    options.push_back({
        .name = "labels",
        .type = "ARRAY<STRUCT<STRING, STRING>>",
        .value = "[" + absl::StrJoin(entries, ", ") + "]",
    });
  }
  return options;
}

void AppendOptions(const Row& identity, std::vector<Option> options, std::vector<Row>& rows) {
  std::ranges::sort(options, {}, &Option::name);
  std::ranges::transform(options, std::back_inserter(rows), [&](const Option& option) {
    return Concat(identity, {String(option.name), String(option.type), String(option.value)});
  });
}

absl::StatusOr<ViewRows> SchemataRows(const ViewContext& context) {
  ViewRows result;
  for (const DatasetReference& dataset : context.datasets) {
    const std::optional<DatasetMetadata> metadata =
        context.source.DescribeDataset(dataset.project_id, dataset.dataset_id);
    if (!metadata.has_value()) {
      // Dropped since it was listed.
      continue;
    }
    const std::string location = metadata->Location();
    result.rows.push_back(Concat(
        Identity(dataset),
        {
            kNull,
            CreationTime(metadata->creation_time),
            kNull,
            String(location),
            String(absl::StrCat("CREATE SCHEMA `", dataset.project_id, ".", dataset.dataset_id,
                                "`\nOPTIONS(\n  location=\"", ToLowerAscii(location), "\"\n);")),
            kNull,
            kNull,
        }));
  }
  return result;
}

absl::StatusOr<ViewRows> SchemataOptionsRows(const ViewContext& context) {
  ViewRows result;
  for (const DatasetReference& dataset : context.datasets) {
    const std::optional<DatasetMetadata> metadata =
        context.source.DescribeDataset(dataset.project_id, dataset.dataset_id);
    if (!metadata.has_value()) {
      continue;
    }
    std::vector<Option> options = CommonOptions(metadata->description, metadata->friendly_name,
                                                metadata->labels, metadata->default_rounding_mode);
    if (metadata->default_table_expiration_ms.has_value()) {
      constexpr double kMillisPerDay = 24.0 * 60 * 60 * 1000;
      options.push_back({
          .name = "default_table_expiration_days",
          .type = "FLOAT64",
          .value = Float64OptionValue(static_cast<double>(*metadata->default_table_expiration_ms) /
                                      kMillisPerDay),
      });
    }
    AppendOptions(Identity(dataset), std::move(options), result.rows);
  }
  return result;
}

absl::StatusOr<ViewRows> TablesRows(const ViewContext& context) {
  ViewRows result;
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
      result.rows.push_back(
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
  return result;
}

absl::StatusOr<ViewRows> TableOptionsRows(const ViewContext& context) {
  ViewRows result;
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
      AppendOptions(Identity(dataset, table), std::move(options), result.rows);
    }
  }
  return result;
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

// Counts the rows of each partition of each table. The emulator keeps no storage statistics, so
// the byte counts are 0, as tables.get reports numBytes, and no partition has a last modified
// time.
absl::StatusOr<ViewRows> PartitionsRows(const ViewContext& context) {
  ViewRows result;
  for (const DatasetReference& dataset : context.datasets) {
    const std::string& project = dataset.project_id;
    for (const std::string& table : context.source.ListTables(project, dataset.dataset_id)) {
      // Views have no partitions.
      const std::optional<TableDescription> description =
          context.source.DescribeTable(project, dataset.dataset_id, table);
      if (!description.has_value()) {
        continue;
      }
      std::vector<std::string> cells;
      std::ranges::transform(Identity(dataset, table), std::back_inserter(cells),
                             [](const Cell& cell) { return *cell; });
      const bool partitioned = description->metadata.partitioned();
      cells.insert(cells.end(), {
                                    partitioned ? "partition_id" : "NULL",
                                    "COUNT(*)",
                                    "0",
                                    "0",
                                    "NULL",
                                    "'ACTIVE'",
                                });
      const std::string reference = QualifiedName(TableReference{
          .project_id = project,
          .dataset_id = dataset.dataset_id,
          .table_id = table,
      });
      // A table that is not partitioned has one partition, even when it is empty.
      result.queries.push_back("SELECT " + absl::StrJoin(cells, ", ") + " FROM (SELECT " +
                               PartitionId(description->metadata) + " AS partition_id FROM " +
                               reference + ")" + (partitioned ? " GROUP BY partition_id" : ""));
    }
  }
  return result;
}

absl::StatusOr<ViewRows> ViewsRows(const ViewContext& context) {
  ViewRows result;
  for (const DatasetReference& dataset : context.datasets) {
    for (const std::string& table :
         context.source.ListTables(dataset.project_id, dataset.dataset_id)) {
      const std::optional<ViewDescription> description =
          context.source.FindView(dataset.project_id, dataset.dataset_id, table);
      if (!description.has_value()) {
        continue;
      }
      result.rows.push_back(
          Concat(Identity(dataset, table), {
                                               OptionalString(description->query),
                                               kNull,
                                               // Views without an emulator definition have an
                                               // unknown SQL dialect too.
                                               description->query.empty() ? kNull : String("YES"),
                                           }));
    }
  }
  return result;
}

absl::StatusOr<ViewRows> ColumnsRows(const ViewContext& context) {
  ViewRows result;
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
        result.rows.push_back(Concat(Identity(dataset, table),
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
  return result;
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

absl::StatusOr<ViewRows> ColumnFieldPathsRows(const ViewContext& context) {
  ViewRows result;
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
                                                   field, context.type_factory, result.rows);
            !status.ok()) {
          return status;
        }
      }
    }
  }
  return result;
}

absl::StatusOr<ViewRows> RoutinesRows(const ViewContext& context) {
  ViewRows result;
  for (const DatasetReference& dataset : context.datasets) {
    for (const Routine& routine : context.source.ListRoutines(dataset)) {
      const auto& resource = routine.resource;
      const Row identity = Identity(dataset, routine.reference.routine_id);
      Row row = Concat(identity, identity);
      const auto type = resource.at("routineType").get<std::string>();
      result.rows.push_back(
          Concat(std::move(row),
                 {
                     String(type == "PROCEDURE"               ? "PROCEDURE"
                            : type == "TABLE_VALUED_FUNCTION" ? "TABLE FUNCTION"
                                                              : "FUNCTION"),
                     resource.contains("returnTableType")
                         ? String(StandardSqlTableTypeName(resource.at("returnTableType")))
                     : resource.contains("returnType")
                         ? String(StandardSqlTypeName(resource.at("returnType")))
                         : kNull,
                     String("SQL"),
                     String(resource.at("definitionBody").get<std::string>()),
                     kNull,
                     kNull,
                     kNull,
                     CreationTime(std::stoll(resource.at("creationTime").get<std::string>())),
                     CreationTime(std::stoll(resource.at("lastModifiedTime").get<std::string>())),
                     String(RoutineDdl(routine)),
                     kNull,
                 }));
    }
  }
  return result;
}

absl::StatusOr<ViewRows> ParametersRows(const ViewContext& context) {
  ViewRows result;
  for (const DatasetReference& dataset : context.datasets) {
    for (const Routine& routine : context.source.ListRoutines(dataset)) {
      const auto& resource = routine.resource;
      const bool procedure = resource.at("routineType") == "PROCEDURE";
      const Row identity = Identity(dataset, routine.reference.routine_id);
      const auto parameter = [&](int64_t ordinal, Cell mode, bool is_result, Cell name, Cell type) {
        result.rows.push_back(Concat(identity, {
                                                   Int(ordinal),
                                                   std::move(mode),
                                                   String(is_result ? "YES" : "NO"),
                                                   std::move(name),
                                                   std::move(type),
                                                   kNull,
                                                   kNull,
                                               }));
      };
      if (resource.contains("returnType")) {
        parameter(0, kNull, true, kNull, String(StandardSqlTypeName(resource.at("returnType"))));
      }
      int64_t ordinal = 0;
      for (const auto& argument : resource.at("arguments")) {
        parameter(++ordinal, procedure ? String(argument.value("mode", "IN")) : kNull, false,
                  String(argument.at("name").get<std::string>()),
                  String(argument.value("argumentKind", "") == "ANY_TYPE"
                             ? "ANY TYPE"
                             : StandardSqlTypeName(argument.at("dataType"))));
      }
    }
  }
  return result;
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
  const std::vector<ViewColumn>& (*columns)();
  absl::StatusOr<ViewRows> (*rows)(const ViewContext&);
};

constexpr std::array kViews = {
    ViewDefinition{
        .name = "SCHEMATA",
        .scope = ViewScope::kProject,
        .columns = SchemataColumns,
        .rows = SchemataRows,
    },
    ViewDefinition{
        .name = "SCHEMATA_OPTIONS",
        .scope = ViewScope::kProject,
        .columns = SchemataOptionsColumns,
        .rows = SchemataOptionsRows,
    },
    ViewDefinition{
        .name = "TABLES",
        .scope = ViewScope::kDatasetOrRegion,
        .columns = TablesColumns,
        .rows = TablesRows,
    },
    ViewDefinition{
        .name = "TABLE_OPTIONS",
        .scope = ViewScope::kDatasetOrRegion,
        .columns = TableOptionsColumns,
        .rows = TableOptionsRows,
    },
    ViewDefinition{
        .name = "PARTITIONS",
        .scope = ViewScope::kDataset,
        .columns = PartitionsColumns,
        .rows = PartitionsRows,
    },
    ViewDefinition{
        .name = "VIEWS",
        .scope = ViewScope::kDatasetOrRegion,
        .columns = ViewsColumns,
        .rows = ViewsRows,
    },
    ViewDefinition{
        .name = "COLUMNS",
        .scope = ViewScope::kDatasetOrRegion,
        .columns = ColumnsColumns,
        .rows = ColumnsRows,
    },
    ViewDefinition{
        .name = "COLUMN_FIELD_PATHS",
        .scope = ViewScope::kDatasetOrRegion,
        .columns = ColumnFieldPathsColumns,
        .rows = ColumnFieldPathsRows,
    },
    ViewDefinition{
        .name = "ROUTINES",
        .scope = ViewScope::kDatasetOrRegion,
        .columns = RoutinesColumns,
        .rows = RoutinesRows,
    },
    ViewDefinition{
        .name = "PARAMETERS",
        .scope = ViewScope::kDatasetOrRegion,
        .columns = ParametersColumns,
        .rows = ParametersRows,
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
        return not_found();
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

  const std::vector<ViewColumn>& columns = definition->columns();
  absl::StatusOr<ViewRows> rows = definition->rows(context);
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
