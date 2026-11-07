#include "src/bigtable_external.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "google/cloud/bigtable/admin/bigtable_table_admin_client.h"
#include "google/cloud/bigtable/admin/bigtable_table_admin_connection.h"
#include "google/cloud/bigtable/data_connection.h"
#include "google/cloud/bigtable/filters.h"
#include "google/cloud/bigtable/instance_resource.h"
#include "google/cloud/bigtable/options.h"
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
    FieldSchema cell{
        .name = "cell",
        .type = FieldType::kRecord,
        .mode =
            family.only_read_latest.value_or(false) ? FieldMode::kNullable : FieldMode::kRepeated,
        .fields =
            {
                {.name = "timestamp", .type = FieldType::kTimestamp},
                {
                    .name = "value",
                    .type = family.type == "STRING" ? FieldType::kString : FieldType::kBytes,
                },
            },
    };
    FieldSchema column{
        .name = "column",
        .type = FieldType::kRecord,
        .mode = FieldMode::kRepeated,
        .fields = {{.name = "name", .type = FieldType::kString}, std::move(cell)},
    };
    return FieldSchema{
        .name = family.family_id,
        .type = FieldType::kRecord,
        .fields = {std::move(column)},
    };
  });
  return schema;
}

TableRead BigtableExternalRead(GcsExternalData definition, std::vector<FieldSchema> schema) {
  return [definition = std::move(definition),
          schema = std::move(schema)](TableReadContext& context) {
    ValidateBigtableExternal(definition);
    const BigtableSource source = ParseBigtableSource(definition);
    google::cloud::Options connection_options;
    if (!source.profile.empty()) {
      connection_options.set<cbt::AppProfileIdOption>(source.profile);
    }
    // Client-side metrics export to Cloud Monitoring, and detecting their resource waits for the
    // GCE metadata server, which made each read take seconds.
    cbt::Table table(
        cbt::MakeDataConnection(
            {cbt::InstanceResource(google::cloud::Project(source.project), source.instance)},
            google::cloud::Options{}.set<cbt::EnableMetricsOption>(false)),
        cbt::TableResource(source.project, source.instance, source.table), connection_options);
    std::string query;
    for (const auto& row :
         table.ReadRows(cbt::RowRange::InfiniteRange(), cbt::Filter::PassAllFilter())) {
      CheckStatus(row.status());
      std::string select = "SELECT ";
      for (size_t i = 0; i < schema.size(); ++i) {
        const auto& field = schema.at(i);
        if (i != 0) {
          select += ',';
        }
        std::string value;
        if (i == 0) {
          value = Bytes(row->row_key(), field.type == FieldType::kString);
        } else if (context.columns && !context.columns->contains(field.name)) {
          value = "NULL";
        } else {
          const bool latest = field.fields.at(0).fields.at(1).mode != FieldMode::kRepeated;
          std::map<std::string, std::vector<std::string>> columns;
          for (const auto& cell : row->cells()) {
            if (cell.family_name() != field.name) {
              continue;
            }
            auto& cells = columns[cell.column_qualifier()];
            if (latest && !cells.empty()) {
              continue;
            }
            cells.push_back("{'timestamp': make_timestamp(" +
                            std::to_string(cell.timestamp().count()) + "), 'value': " +
                            Bytes(cell.value(), field.fields.at(0).fields.at(1).fields.at(1).type ==
                                                    FieldType::kString) +
                            "}");
          }
          if (columns.empty()) {
            throw ApiError::Unsupported(
                "Reading a Bigtable row with a missing selected column family");
          }
          std::vector<std::string> values;
          values.reserve(columns.size());
          for (const auto& [name, cells] : columns) {
            values.push_back("{'name': " + Bytes(name, true) +
                             ", 'cell': " + (latest ? cells.front() : List(cells)) + "}");
          }
          value = "{'column': " + List(values) + "}";
        }
        select += "CAST(" + value + " AS " + ToDuckDbReadType(field) + ") AS " +
                  QuoteIdentifier(field.name);
      }
      if (!query.empty()) {
        query += " UNION ALL ";
      }
      query += select;
    }
    if (query.empty()) {
      query = "SELECT ";
      for (size_t i = 0; i < schema.size(); ++i) {
        if (i != 0) {
          query += ',';
        }
        query += "CAST(NULL AS " + ToDuckDbReadType(schema.at(i)) + ") AS " +
                 QuoteIdentifier(schema.at(i).name);
      }
      query += " WHERE FALSE";
    }
    return query;
  };
}

}  // namespace bigquery_emulator_duckdb
