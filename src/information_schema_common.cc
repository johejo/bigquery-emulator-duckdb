#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/str_join.h"
#include "absl/time/time.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/strings.h"
#include "googlesql/public/type.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/information_schema_internal.h"
#include "src/references.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::information_schema_internal {

FieldSchema ColumnField(const ViewColumn& column) {
  return {
      .name = column.name,
      .type = column.type,
      .mode = column.repeated ? FieldMode::kRepeated : FieldMode::kNullable,
  };
}

Cell String(const std::string& value) { return QuoteLiteral(value); }
Cell Int(int64_t value) { return std::to_string(value); }

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

}  // namespace bigquery_emulator_duckdb::information_schema_internal
