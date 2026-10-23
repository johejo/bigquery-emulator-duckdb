#include "src/information_schema_datasets.h"

#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "src/duckdb_sql.h"
#include "src/information_schema_options.h"
#include "src/information_schema_rows.h"
#include "src/information_schema_view.h"
#include "src/references.h"
#include "src/table_metadata.h"
#include "src/table_source.h"

namespace bigquery_emulator_duckdb::information_schema {

namespace {

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

}  // namespace

absl::StatusOr<std::vector<Row>> SchemataRows(const ViewContext& context) {
  std::vector<Row> rows;
  for (const DatasetReference& dataset : context.datasets) {
    const std::optional<DatasetMetadata> metadata =
        context.source.DescribeDataset(dataset.project_id, dataset.dataset_id);
    if (!metadata.has_value()) {
      // Dropped since it was listed.
      continue;
    }
    const std::string location = metadata->Location();
    rows.push_back(Concat(
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
  return rows;
}

absl::StatusOr<std::vector<Row>> SchemataOptionsRows(const ViewContext& context) {
  std::vector<Row> rows;
  for (const DatasetReference& dataset : context.datasets) {
    const std::optional<DatasetMetadata> metadata =
        context.source.DescribeDataset(dataset.project_id, dataset.dataset_id);
    if (!metadata.has_value()) {
      continue;
    }
    std::vector<Option> options =
        CommonOptions(metadata->description, metadata->friendly_name, metadata->labels,
                      metadata->default_rounding_mode, metadata->label_order);
    // Every dataset lists its location, in lower case as SCHEMATA's DDL spells it.
    options.push_back({
        .name = "location",
        .type = "STRING",
        .value = StringOptionValue(ToLowerAscii(metadata->Location())),
    });
    if (metadata->default_table_expiration_ms.has_value()) {
      constexpr double kMillisPerDay = 24.0 * 60 * 60 * 1000;
      options.push_back({
          .name = "default_table_expiration_days",
          .type = "FLOAT64",
          .value = Float64OptionValue(static_cast<double>(*metadata->default_table_expiration_ms) /
                                      kMillisPerDay),
      });
    }
    AppendOptions(Identity(dataset), std::move(options), rows);
  }
  return rows;
}

}  // namespace bigquery_emulator_duckdb::information_schema
