#include "src/bigtable_external.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
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
#include "googlesql/common/utf_util.h"
#include "src/api_error.h"
#include "src/bigtable_options.h"
#include "src/duckdb_sql.h"
#include "src/external_values.h"
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
  // The external table's name, which BigQuery's read errors give.
  std::string_view table;
  const FieldSchema& schema;
  // A read of `column` counts every cell in the family, though its array excludes promoted cells.
  const std::set<size_t>* fields;
  std::map<std::string_view, size_t> promoted;
  // Whether each field's cells use TEXT encoding, indexed like schema.fields.
  std::vector<bool> text;

  [[nodiscard]] bool Reads(size_t field) const {
    return fields == nullptr || fields->contains(field);
  }
};

uint64_t BigEndianBits(std::string_view value) {
  return std::accumulate(value.begin(), value.end(), uint64_t{0}, [](uint64_t prefix, char byte) {
    return (prefix << 8U) | static_cast<unsigned char>(byte);
  });
}

[[noreturn]] void ThrowCellSize(std::string_view value, size_t expected) {
  throw ApiError::Invalid("Incorrect cell value size. Expected: " + std::to_string(expected) +
                          ", Actual: " + std::to_string(value.size()));
}

std::string DoubleLiteral(double number) {
  std::array<char, 32> text{};
  const auto result = std::to_chars(text.begin(), text.end(), number);
  // The string cast spells NaN, infinities and -0, which DuckDB numeric literals cannot.
  return "CAST('" + std::string(text.begin(), result.ptr) + "' AS DOUBLE)";
}

// BigQuery's parsing of TEXT cells matches absl::SimpleAtoi and absl::SimpleAtob, and strtod
// after stripping ASCII whitespace, which also accepts hexadecimal floats and overflows to
// infinity.
std::string TextCellLiteral(std::string_view value, FieldType type) {
  switch (type) {
    case FieldType::kInteger:
      if (int64_t number = 0; absl::SimpleAtoi(value, &number)) {
        return std::to_string(number);
      }
      break;
    case FieldType::kFloat: {
      const std::string text(absl::StripAsciiWhitespace(value));
      char* end = nullptr;
      const double number = std::strtod(text.c_str(), &end);
      if (!text.empty() && end == text.c_str() + text.size()) {
        return DoubleLiteral(number);
      }
      break;
    }
    case FieldType::kBoolean:
      if (bool flag = false; absl::SimpleAtob(value, &flag)) {
        return flag ? "true" : "false";
      }
      break;
    default:
      return Bytes(value, type == FieldType::kString);
  }
  throw ApiError::Invalid("Could not convert cell value " + std::string(value) + ".");
}

// A JSON cell's text, whatever its encoding, as BigQuery's answers in
// bigtable_json.api.answers.json show. BigQuery fails a query that reads an invalid document with
// nlohmann's message, and a number out of range with a suffix of its own.
std::string JsonCellLiteral(std::string_view value) {
  const auto json = ExternalValue(FieldType::kJson, value);
  if (!json.ok()) {
    std::string message(json.status().message());
    if (message.starts_with("number overflow")) {
      message += "; error in JSON_TYPE expression";
    }
    throw ApiError::InvalidQuery(message);
  }
  return QuoteLiteral(*json);
}

// BINARY cells hold HBase Bytes.toBytes encodings, as BigtableColumnFamily.type documents.
std::string CellLiteral(std::string_view value, FieldType type, bool text) {
  if (type == FieldType::kJson) {
    return JsonCellLiteral(value);
  }
  if (text) {
    return TextCellLiteral(value, type);
  }
  switch (type) {
    case FieldType::kInteger: {
      // BigQuery accepts both HBase Bytes.toBytes(int) and Bytes.toBytes(long).
      if (value.size() != sizeof(int32_t) && value.size() != sizeof(int64_t)) {
        ThrowCellSize(value, sizeof(int64_t));
      }
      const uint64_t bits = BigEndianBits(value);
      return value.size() == sizeof(int32_t)
                 ? std::to_string(std::bit_cast<int32_t>(static_cast<uint32_t>(bits)))
                 : std::to_string(std::bit_cast<int64_t>(bits));
    }
    case FieldType::kFloat: {
      // BigQuery accepts both HBase Bytes.toBytes(float) and Bytes.toBytes(double).
      if (value.size() != sizeof(float) && value.size() != sizeof(double)) {
        ThrowCellSize(value, sizeof(double));
      }
      const uint64_t bits = BigEndianBits(value);
      return DoubleLiteral(value.size() == sizeof(float)
                               ? std::bit_cast<float>(static_cast<uint32_t>(bits))
                               : std::bit_cast<double>(bits));
    }
    case FieldType::kBoolean:
      // As HBase Bytes.toBoolean does, BigQuery reads any nonzero byte as true.
      if (value.size() != 1) {
        ThrowCellSize(value, 1);
      }
      return value.front() == '\x00' ? "false" : "true";
    default:
      return Bytes(value, type == FieldType::kString);
  }
}

void AppendCell(std::vector<std::string>& versions, const cbt::Cell& cell,
                const FieldSchema& schema, bool text) {
  // ReadRows orders versions newest first within each qualifier.
  if (schema.mode != FieldMode::kRepeated && !versions.empty()) {
    return;
  }
  versions.push_back("{'timestamp': make_timestamp(" + std::to_string(cell.timestamp().count()) +
                     "), 'value': " + CellLiteral(cell.value(), schema.fields.at(1).type, text) +
                     "}");
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
  const std::string* previous_qualifier = nullptr;
  for (const auto& cell : row.cells()) {
    if (cell.family_name() != read.schema.name) {
      continue;
    }
    present |= fallback;
    // ReadRows orders a family's cells by qualifier, and versions newest first.
    const bool newest =
        previous_qualifier == nullptr || *previous_qualifier != cell.column_qualifier();
    previous_qualifier = &cell.column_qualifier();
    const auto found = read.promoted.find(cell.column_qualifier());
    const FieldSchema& cell_schema =
        found == read.promoted.end() ? fallback_cell : fields.at(found->second).fields.front();
    // BigQuery checks the UTF-8 of each STRING or JSON version it reads, of an unlisted cell even
    // when the query reads none, but not of an unread named column or a version onlyReadLatest
    // leaves out, as bigtable_json.api.answers.json and the STRING versions TestBigtableExternal
    // reads show.
    if ((found == read.promoted.end() || read.Reads(found->second)) &&
        (newest || cell_schema.mode == FieldMode::kRepeated)) {
      const FieldType type = cell_schema.fields.back().type;
      if ((type == FieldType::kString || type == FieldType::kJson) &&
          !googlesql::IsWellFormedUTF8(cell.value())) {
        throw ApiError::Invalid(
            "Error while reading table: " + std::string(read.table) +
            ", error message: Invalid utf-8 encoding found At row:" + row.row_key() +
            ", column_family:" + read.schema.name + ", column:" + cell.column_qualifier());
      }
    }
    if (found != read.promoted.end()) {
      const size_t field = found->second;
      if (read.Reads(field)) {
        present = true;
        AppendCell(named.at(field), cell, fields.at(field).fields.front(), read.text.at(field));
      }
    } else if (fallback) {
      AppendCell(unlisted[cell.column_qualifier()], cell, fallback_cell, read.text.front());
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

TableRead BigtableExternalRead(GcsExternalData definition, std::vector<FieldSchema> schema,
                               std::string table_name) {
  return FieldProjectedRead{
      .prepare =
          [definition = std::move(definition), schema = std::move(schema),
           table_name = std::move(table_name)](TableReadContext& context) {
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
                  .table = table_name,
                  .schema = field,
                  .fields = selected == context.fields.end() ? nullptr : &selected->second,
                  .promoted = {},
                  .text = std::vector<bool>(field.fields.size(), false),
              };
              if (definition.bigtable_options) {
                const auto& families = definition.bigtable_options->column_families;
                const auto family =
                    std::ranges::find(families, field.name, &BigtableFamily::family_id);
                if (family != families.end()) {
                  read.text.front() = family->cells.encoding == "TEXT";
                  for (size_t j = 0; j < family->columns.size(); ++j) {
                    const auto& column = family->columns.at(j);
                    read.promoted.emplace(column.qualifier, j + 1);
                    read.text.at(j + 1) = column.cells.encoding.value_or(
                                              family->cells.encoding.value_or("")) == "TEXT";
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
