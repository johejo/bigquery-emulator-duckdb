#include "src/bigtable_external.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
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
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/bigtable_options.h"
#include "src/duckdb_sql.h"
#include "src/external_values.h"
#include "src/field_schema.h"
#include "src/key_ranges.h"
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
  // The family's struct, which an outputColumnFamiliesAsJson read builds JSON in the shape of.
  const FieldSchema& schema;
  bool json = false;
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

[[noreturn]] void ThrowConversion(std::string_view value) {
  throw ApiError::Invalid("Could not convert cell value " + std::string(value) + ".");
}

// BINARY cells hold HBase Bytes.toBytes encodings, as BigtableColumnFamily.type documents.
// BigQuery's parsing of TEXT cells matches absl::SimpleAtoi and absl::SimpleAtob, and strtod
// after stripping ASCII whitespace, which also accepts hexadecimal floats and overflows to
// infinity.
int64_t IntegerCell(std::string_view value, bool text) {
  if (text) {
    int64_t number = 0;
    if (!absl::SimpleAtoi(value, &number)) {
      ThrowConversion(value);
    }
    return number;
  }
  // BigQuery accepts both HBase Bytes.toBytes(int) and Bytes.toBytes(long).
  if (value.size() != sizeof(int32_t) && value.size() != sizeof(int64_t)) {
    ThrowCellSize(value, sizeof(int64_t));
  }
  const uint64_t bits = BigEndianBits(value);
  return value.size() == sizeof(int32_t) ? std::bit_cast<int32_t>(static_cast<uint32_t>(bits))
                                         : std::bit_cast<int64_t>(bits);
}

double FloatCell(std::string_view value, bool text) {
  if (text) {
    const std::string trimmed(absl::StripAsciiWhitespace(value));
    char* end = nullptr;
    const double number = std::strtod(trimmed.c_str(), &end);
    if (trimmed.empty() || end != trimmed.c_str() + trimmed.size()) {
      ThrowConversion(value);
    }
    return number;
  }
  // BigQuery accepts both HBase Bytes.toBytes(float) and Bytes.toBytes(double).
  if (value.size() != sizeof(float) && value.size() != sizeof(double)) {
    ThrowCellSize(value, sizeof(double));
  }
  const uint64_t bits = BigEndianBits(value);
  return value.size() == sizeof(float) ? std::bit_cast<float>(static_cast<uint32_t>(bits))
                                       : std::bit_cast<double>(bits);
}

bool BooleanCell(std::string_view value, bool text) {
  if (text) {
    bool flag = false;
    if (!absl::SimpleAtob(value, &flag)) {
      ThrowConversion(value);
    }
    return flag;
  }
  // As HBase Bytes.toBoolean does, BigQuery reads any nonzero byte as true.
  if (value.size() != 1) {
    ThrowCellSize(value, 1);
  }
  return value.front() != '\x00';
}

// A JSON cell's text, whatever its encoding, as BigQuery's answers in
// bigtable_json.api.answers.json show. BigQuery fails a query that reads an invalid document with
// nlohmann's message, and a number out of range with a suffix of its own.
std::string JsonCellText(std::string_view value) {
  auto json = ExternalValue(FieldType::kJson, value);
  if (!json.ok()) {
    std::string message(json.status().message());
    if (message.starts_with("number overflow")) {
      message += "; error in JSON_TYPE expression";
    }
    throw ApiError::InvalidQuery(message);
  }
  return *std::move(json);
}

std::string CellLiteral(std::string_view value, FieldType type, bool text) {
  switch (type) {
    case FieldType::kJson:
      return QuoteLiteral(JsonCellText(value));
    case FieldType::kInteger:
      return std::to_string(IntegerCell(value, text));
    case FieldType::kFloat:
      return DoubleLiteral(FloatCell(value, text));
    case FieldType::kBoolean:
      return BooleanCell(value, text) ? "true" : "false";
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
        throw ApiError::Invalid("Error while reading table: " + std::string(read.table) +
                                ", error message: Invalid utf-8 encoding found At row:" +
                                googlesql::CoerceToWellFormedUTF8(row.row_key()) +
                                ", column_family:" + read.schema.name +
                                ", column:" + cell.column_qualifier());
      }
    }
    if (found != read.promoted.end()) {
      const size_t field = found->second;
      if (read.Reads(field)) {
        present = true;
        AppendCell(named.at(field), cell, fields.at(field).fields.front(), read.text.at(field));
      }
    } else if (fallback) {
      // The qualifier is the column's STRING name, which BigQuery gives with its bytes as they
      // are, as bigtable_qualifier_names.api.yaml shows, but a DuckDB string cannot hold.
      // Checking it here fails a read of the row even when a filter on rowkey drops it.
      if (!googlesql::IsWellFormedUTF8(cell.column_qualifier())) {
        throw ApiError::Unsupported("Bigtable qualifiers that are not valid UTF-8 as names of " +
                                    read.schema.name + ".column");
      }
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

// A family's struct, or the shape of its JSON with outputColumnFamiliesAsJson.
FieldSchema FamilySchema(const BigtableFamily& family) {
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
}

// With outputColumnFamiliesAsJson, a family is a JSON object keyed by qualifier, or by field name
// for a named column, of {timestamp, value} versions newest first, or of the newest alone where
// onlyReadLatest applies. A value is a string for BYTES and STRING cells and a JSON value for the
// others, as bigtable_json_families.api.answers.json and
// bigtable_json_families_types.api.answers.json show.
std::optional<std::string> ReadJsonFamily(const FamilyRead& read, const cbt::Row& row) {
  const auto& fields = read.schema.fields;
  nlohmann::json result = nlohmann::json::object();
  bool present = false;
  const std::string* previous_qualifier = nullptr;
  for (const auto& cell : row.cells()) {
    if (cell.family_name() != read.schema.name) {
      continue;
    }
    present = true;
    const bool newest =
        previous_qualifier == nullptr || *previous_qualifier != cell.column_qualifier();
    previous_qualifier = &cell.column_qualifier();
    const auto found = read.promoted.find(cell.column_qualifier());
    const size_t field = found == read.promoted.end() ? 0 : found->second;
    const FieldSchema& cell_schema =
        field == 0 ? fields.front().fields.at(1) : fields.at(field).fields.front();
    const bool repeated = cell_schema.mode == FieldMode::kRepeated;
    if (!newest && !repeated) {
      continue;
    }
    const FieldType type = cell_schema.fields.back().type;
    // BigQuery checks the cells it gives as text. Its message also names the Bigtable cluster
    // that served the read.
    if ((type == FieldType::kBytes || type == FieldType::kString || type == FieldType::kJson) &&
        !googlesql::IsWellFormedUTF8(cell.value())) {
      throw ApiError::Invalid(
          "Error while reading table: " + std::string(read.table) +
          ", error message: Invalid utf-8 encoding found at column_family: " + read.schema.name);
    }
    const bool text = read.text.at(field);
    nlohmann::json value;
    switch (type) {
      case FieldType::kInteger:
        value = IntegerCell(cell.value(), text);
        break;
      case FieldType::kFloat:
        // JSON has no NaN or infinities, which BigQuery gives as null.
        if (const double number = FloatCell(cell.value(), text); std::isfinite(number)) {
          value = number;
        }
        break;
      case FieldType::kBoolean:
        value = BooleanCell(cell.value(), text);
        break;
      case FieldType::kJson:
        value = nlohmann::json::parse(JsonCellText(cell.value()));
        break;
      default:
        value = cell.value();
    }
    nlohmann::json version = {{"timestamp", cell.timestamp().count()}, {"value", std::move(value)}};
    // BigQuery replaces what is not UTF-8 in a qualifier with U+FFFD.
    auto& entry = result[field == 0 ? googlesql::CoerceToWellFormedUTF8(cell.column_qualifier())
                                    : fields.at(field).name];
    if (repeated) {
      entry.push_back(std::move(version));
    } else {
      entry = std::move(version);
    }
  }
  if (!present) {
    return std::nullopt;
  }
  return QuoteLiteral(result.dump());
}

// A regular expression that matches exactly a family ID, which holds only ASCII letters, digits,
// '_', '-' and '.'.
std::string QuoteFamilyRegex(std::string_view family) {
  std::string result;
  for (const char byte : family) {
    if (!absl::ascii_isalnum(static_cast<unsigned char>(byte)) && byte != '_') {
      result += '\\';
    }
    result += byte;
  }
  return result;
}

// The cells ReadFamily needs of a family. Older versions of a column matter only to a repeated
// cell field: ReadFamily reads only the newest of the others, and so checks only its UTF-8.
cbt::Filter FamilyFilter(const FamilyRead& read) {
  const auto& fields = read.schema.fields;
  // `column` holds its cell after `name`; a named field holds only its cell.
  const auto repeated = [&](size_t field) {
    return fields.at(field).fields.back().mode == FieldMode::kRepeated;
  };
  // ReadFamily checks unlisted cells whether or not the query reads them.
  bool latest = !repeated(0);
  for (size_t i = 1; i < fields.size(); ++i) {
    latest &= !read.Reads(i) || !repeated(i);
  }
  auto family = cbt::Filter::FamilyRegex(QuoteFamilyRegex(read.schema.name));
  return latest ? cbt::Filter::Chain(std::move(family), cbt::Filter::Latest(1)) : family;
}

// Reading the referenced families alone keeps a scan from transferring the rest of each row.
cbt::Filter ReadFilter(const std::map<size_t, FamilyRead>& reads) {
  if (reads.empty()) {
    // Every row has a cell, and a scan that references no family includes every row.
    return cbt::Filter::Chain(cbt::Filter::CellsRowLimit(1), cbt::Filter::StripValueTransformer());
  }
  std::vector<cbt::Filter> filters;
  filters.reserve(reads.size());
  std::ranges::transform(reads | std::views::values, std::back_inserter(filters), FamilyFilter);
  return filters.size() == 1 ? std::move(filters.front())
                             : cbt::Filter::InterleaveFromRange(filters.begin(), filters.end());
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
  const bool json = options.output_column_families_as_json.value_or(false);
  std::ranges::transform(
      families, std::back_inserter(schema), [json](const BigtableFamily& family) {
        return json ? FieldSchema{.name = family.family_id, .type = FieldType::kJson}
                    : FamilySchema(family);
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
            const bool json =
                definition.bigtable_options &&
                definition.bigtable_options->output_column_families_as_json.value_or(false);
            // The structs that JSON families are read in the shape of, which reads refer to.
            std::map<size_t, FieldSchema> layouts;
            std::map<size_t, FamilyRead> reads;
            for (size_t i = 1; i < schema.size(); ++i) {
              const auto& field = schema.at(i);
              if (context.columns && !context.columns->contains(field.name)) {
                continue;
              }
              const BigtableFamily* family = nullptr;
              if (definition.bigtable_options) {
                const auto& families = definition.bigtable_options->column_families;
                const auto found =
                    std::ranges::find(families, field.name, &BigtableFamily::family_id);
                family = found == families.end() ? nullptr : &*found;
              }
              const FieldSchema& layout =
                  json
                      ? layouts
                            .emplace(i, FamilySchema(family != nullptr
                                                         ? *family
                                                         : BigtableFamily{.family_id = field.name}))
                            .first->second
                      : field;
              const auto selected = context.fields.find(field.name);
              FamilyRead read{
                  .table = table_name,
                  .schema = layout,
                  .json = json,
                  .fields = json || selected == context.fields.end() ? nullptr : &selected->second,
                  .promoted = {},
                  .text = std::vector<bool>(layout.fields.size(), false),
              };
              if (family != nullptr) {
                read.text.front() = family->cells.encoding == "TEXT";
                for (size_t j = 0; j < family->columns.size(); ++j) {
                  const auto& column = family->columns.at(j);
                  read.promoted.emplace(column.qualifier, j + 1);
                  read.text.at(j + 1) =
                      column.cells.encoding.value_or(family->cells.encoding.value_or("")) == "TEXT";
                }
              }
              reads.emplace(i, std::move(read));
            }
            std::vector<std::string> values(schema.size(), "NULL");
            std::string query;
            const bool text_key = schema.front().type == FieldType::kString;
            // Every row is read and checked, since BigQuery fails a query on a cell it cannot
            // read in a row that a filter on rowkey drops, as bigtable_row_key_filters.api.yaml
            // shows. Only the rows in the filters' key ranges reach the query.
            for (const auto& row :
                 table.ReadRows(cbt::RowRange::InfiniteRange(), ReadFilter(reads))) {
              CheckStatus(row.status());
              // BigQuery keeps rows matching any read, or all rows when no family is referenced.
              bool present = reads.empty();
              for (const auto& [i, read] : reads) {
                auto value = read.json ? ReadJsonFamily(read, *row) : ReadFamily(read, *row);
                present |= value.has_value();
                values.at(i) = value ? *std::move(value) : "NULL";
              }
              if (present && text_key && !googlesql::IsWellFormedUTF8(row->row_key())) {
                // As bigtable_utf8_keys.api.yaml shows, without the Bigtable cluster that
                // BigQuery's message also names.
                throw ApiError::Invalid("Error while reading table: " + table_name +
                                        ", error message: Invalid utf-8 encoding found At row:" +
                                        googlesql::CoerceToWellFormedUTF8(row->row_key()) +
                                        ", column:rowkey");
              }
              if (present && (!context.keys || KeyRangesContain(*context.keys, row->row_key()))) {
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
      .key_column = "rowkey",
  };
}

}  // namespace bigquery_emulator_duckdb
