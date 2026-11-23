#include "src/bigtable_external.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "google/cloud/bigtable/admin/bigtable_table_admin_client.h"
#include "google/cloud/bigtable/admin/bigtable_table_admin_connection.h"
#include "google/cloud/bigtable/cell.h"
#include "google/cloud/bigtable/data_connection.h"
#include "google/cloud/bigtable/filters.h"
#include "google/cloud/bigtable/instance_resource.h"
#include "google/cloud/bigtable/options.h"
#include "google/cloud/bigtable/row.h"
#include "google/cloud/bigtable/row_range.h"
#include "google/cloud/bigtable/table.h"
#include "google/cloud/bigtable/table_resource.h"
#include "google/cloud/options.h"
#include "google/cloud/project.h"
#include "google/cloud/status.h"
#include "src/api_error.h"
#include "src/bigtable_options.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/schema_sql.h"
#include "src/statement_resources.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {
namespace {
namespace cbt = google::cloud::bigtable;

void CheckStatus(const google::cloud::Status& status) {
  if (status.ok()) {
    return;
  }
  if (status.code() == google::cloud::StatusCode::kNotFound) {
    throw ApiError::NotFound(status.message());
  }
  throw ApiError::InvalidQuery("Bigtable: " + status.message());
}

std::string Bytes(std::string_view value, bool text) {
  const std::string blob = "from_hex('" + ToHex(value) + "')";
  return text ? "decode(" + blob + ")" : blob;
}

std::string List(const std::vector<std::string>& values) {
  std::string result = "[";
  for (const auto& value : values) {
    if (result.size() > 1) {
      result += ',';
    }
    result += value;
  }
  return result + "]";
}

FieldSchema CellSchema(const BigtableCellOptions& defaults,
                       const BigtableCellOptions& overrides = {}) {
  const bool latest =
      overrides.only_read_latest.value_or(defaults.only_read_latest.value_or(false));
  const auto& type = overrides.type ? overrides.type : defaults.type;
  const auto value_type = type ? ParseFieldType(*type) : std::nullopt;
  return {
      .name = "cell",
      .type = FieldType::kRecord,
      .mode = latest ? FieldMode::kNullable : FieldMode::kRepeated,
      .fields =
          {
              {.name = "timestamp", .type = FieldType::kTimestamp},
              {.name = "value", .type = value_type.value_or(FieldType::kBytes)},
          },
  };
}

struct FamilyRead {
  const FieldSchema& schema;
  // A read of `column` counts every cell in the family, though its array excludes promoted cells.
  const std::set<size_t>* fields;
  std::map<std::string_view, size_t> promoted;

  [[nodiscard]] bool Reads(size_t field) const {
    return fields == nullptr || fields->contains(field);
  }
};

std::string CellLiteral(std::string_view value, FieldType type) {
  if (type != FieldType::kInteger) {
    return Bytes(value, type == FieldType::kString);
  }
  // BigQuery accepts both HBase Bytes.toBytes(int) and Bytes.toBytes(long).
  if (value.size() != sizeof(int32_t) && value.size() != sizeof(int64_t)) {
    throw ApiError::Invalid("Incorrect cell value size. Expected: 8, Actual: " +
                            std::to_string(value.size()));
  }
  const uint64_t bits = std::accumulate(
      value.begin(), value.end(), uint64_t{0},
      [](uint64_t prefix, char byte) { return (prefix << 8U) | static_cast<unsigned char>(byte); });
  return value.size() == sizeof(int32_t)
             ? std::to_string(std::bit_cast<int32_t>(static_cast<uint32_t>(bits)))
             : std::to_string(std::bit_cast<int64_t>(bits));
}

void AppendCell(std::vector<std::string>& versions, const cbt::Cell& cell,
                const FieldSchema& schema) {
  // ReadRows orders versions newest first within each qualifier.
  if (schema.mode != FieldMode::kRepeated && !versions.empty()) {
    return;
  }
  versions.push_back("{'timestamp': make_timestamp(" + std::to_string(cell.timestamp().count()) +
                     "), 'value': " + CellLiteral(cell.value(), schema.fields.at(1).type) + "}");
}

std::string CellValue(const std::vector<std::string>& versions, const FieldSchema& schema) {
  return schema.mode == FieldMode::kRepeated ? List(versions) : versions.front();
}

std::optional<std::string> ReadFamily(const FamilyRead& read, const cbt::Row& row) {
  const bool fallback = read.Reads(0);
  const auto& fields = read.schema.fields;
  const auto& fallback_cell = fields.front().fields.at(1);
  std::map<std::string, std::vector<std::string>> unlisted;
  std::vector<std::vector<std::string>> named(fields.size());
  bool present = false;
  for (const auto& cell : row.cells()) {
    if (cell.family_name() != read.schema.name) {
      continue;
    }
    present |= fallback;
    if (const auto found = read.promoted.find(cell.column_qualifier());
        found != read.promoted.end()) {
      const size_t field = found->second;
      if (read.Reads(field)) {
        present = true;
        AppendCell(named.at(field), cell, fields.at(field).fields.front());
      }
    } else if (fallback) {
      AppendCell(unlisted[cell.column_qualifier()], cell, fallback_cell);
    }
  }
  if (!present) {
    return std::nullopt;
  }
  std::vector<std::string> columns;
  columns.reserve(unlisted.size());
  for (const auto& [qualifier, cells] : unlisted) {
    columns.push_back("{'name': " + Bytes(qualifier, true) +
                      ", 'cell': " + CellValue(cells, fallback_cell) + "}");
  }
  std::string result = "{'column': " + List(columns);
  for (size_t i = 1; i < fields.size(); ++i) {
    result += ", " + QuoteLiteral(fields.at(i).name) + ": ";
    const auto& cells = named.at(i);
    result +=
        cells.empty() ? "NULL" : "{'cell': " + CellValue(cells, fields.at(i).fields.front()) + "}";
  }
  return result + "}";
}

void AppendReadRow(std::string& query, const std::vector<FieldSchema>& schema,
                   const std::vector<std::string>& values) {
  query += query.empty() ? "SELECT " : " UNION ALL SELECT ";
  for (size_t i = 0; i < schema.size(); ++i) {
    if (i != 0) {
      query += ',';
    }
    const auto& field = schema.at(i);
    query += "CAST(" + values.at(i) + " AS " + ToDuckDbReadType(field) + ") AS " +
             QuoteIdentifier(field.name);
  }
}

}  // namespace

std::vector<FieldSchema> DetectBigtableSchema(const GcsExternalData& definition) {
  ValidateBigtableExternal(definition);
  const BigtableSource source = ParseBigtableSource(definition);
  const BigtableOptions options = definition.bigtable_options.value_or(BigtableOptions{});
  std::vector<BigtableFamily> families = options.column_families;
  if (!options.ignore_unspecified_column_families.value_or(false)) {
    namespace admin = google::cloud::bigtable_admin;
    admin::BigtableTableAdminClient client(admin::MakeBigtableTableAdminConnection());
    const auto table = client.GetTable(
        cbt::TableResource(source.project, source.instance, source.table).FullName());
    CheckStatus(table.status());
    for (const auto& [name, family] : table->column_families()) {
      if (std::ranges::find(families, name, &BigtableFamily::family_id) == families.end()) {
        families.push_back(BigtableFamily{.family_id = name});
      }
    }
  }
  std::vector<FieldSchema> schema{
      {
          .name = "rowkey",
          .type = options.read_rowkey_as_string.value_or(false) ? FieldType::kString
                                                                : FieldType::kBytes,
      },
  };
  std::ranges::transform(families, std::back_inserter(schema), [](const BigtableFamily& family) {
    FieldSchema result{
        .name = family.family_id,
        .type = FieldType::kRecord,
        .fields =
            {
                {
                    .name = "column",
                    .type = FieldType::kRecord,
                    .mode = FieldMode::kRepeated,
                    .fields =
                        {
                            {.name = "name", .type = FieldType::kString},
                            CellSchema(family.cells),
                        },
                },
            },
    };
    for (const auto& column : family.columns) {
      result.fields.push_back({
          .name = column.Name(),
          .type = FieldType::kRecord,
          .fields = {CellSchema(family.cells, column.cells)},
      });
    }
    return result;
  });
  SetModesGiven(schema);
  return schema;
}

TableRead BigtableExternalRead(GcsExternalData definition, std::vector<FieldSchema> schema) {
  return FieldProjectedRead{
      .prepare =
          [definition = std::move(definition),
           schema = std::move(schema)](TableReadContext& context) {
            ValidateBigtableExternal(definition);
            const BigtableSource source = ParseBigtableSource(definition);
            google::cloud::Options connection_options;
            if (!source.profile.empty()) {
              connection_options.set<cbt::AppProfileIdOption>(source.profile);
            }
            // Client-side metrics export to Cloud Monitoring, and detecting their resource waits
            // for the GCE metadata server, which made each read take seconds.
            const cbt::InstanceResource instance(google::cloud::Project(source.project),
                                                 source.instance);
            cbt::Table table(
                cbt::MakeDataConnection(
                    {instance}, google::cloud::Options{}.set<cbt::EnableMetricsOption>(false)),
                cbt::TableResource(source.project, source.instance, source.table),
                connection_options);
            std::map<size_t, FamilyRead> reads;
            for (size_t i = 1; i < schema.size(); ++i) {
              const auto& field = schema.at(i);
              if (context.columns && !context.columns->contains(field.name)) {
                continue;
              }
              const auto selected = context.fields.find(field.name);
              FamilyRead read{
                  .schema = field,
                  .fields = selected == context.fields.end() ? nullptr : &selected->second,
                  .promoted = {},
              };
              if (definition.bigtable_options) {
                const auto& families = definition.bigtable_options->column_families;
                const auto family =
                    std::ranges::find(families, field.name, &BigtableFamily::family_id);
                if (family != families.end()) {
                  for (size_t j = 0; j < family->columns.size(); ++j) {
                    read.promoted.emplace(family->columns.at(j).qualifier, j + 1);
                  }
                }
              }
              reads.emplace(i, std::move(read));
            }
            std::vector<std::string> values(schema.size(), "NULL");
            std::string query;
            for (const auto& row :
                 table.ReadRows(cbt::RowRange::InfiniteRange(), cbt::Filter::PassAllFilter())) {
              CheckStatus(row.status());
              // BigQuery keeps rows matching any read, or all rows when no family is referenced.
              bool present = reads.empty();
              for (const auto& [i, read] : reads) {
                auto value = ReadFamily(read, *row);
                present |= value.has_value();
                values.at(i) = value ? *std::move(value) : "NULL";
              }
              if (present) {
                values.front() = Bytes(row->row_key(), schema.front().type == FieldType::kString);
                AppendReadRow(query, schema, values);
              }
            }
            if (query.empty()) {
              std::ranges::fill(values, "NULL");
              AppendReadRow(query, schema, values);
              query += " WHERE FALSE";
            }
            return query;
          },
  };
}

}  // namespace bigquery_emulator_duckdb
