#include "src/bigtable_options.h"

#include <algorithm>
#include <regex>
#include <set>
#include <string>
#include <string_view>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/duckdb_sql.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

BigtableOptions ParseBigtableOptions(std::string_view json) {
  const auto parsed = nlohmann::json::parse(json, nullptr, false);
  if (parsed.is_discarded()) {
    throw ApiError::InvalidQuery("Invalid bigtable_options JSON");
  }
  nlohmann::json configuration;
  configuration["externalDataConfiguration"] = {
      {"sourceFormat", "BIGTABLE"},
      {"sourceUris", {"unused"}},
      {"bigtableOptions", parsed},
  };
  const auto settings = TableMetadataFromJson(configuration);
  if (!settings.external_data || !settings.external_data->bigtable_options) {
    throw ApiError::InvalidQuery("Invalid bigtable_options");
  }
  return *settings.external_data->bigtable_options;
}

BigtableSource ParseBigtableSource(const GcsExternalData& definition) {
  static const std::regex pattern(
      R"(^https://googleapis\.com/bigtable/projects/([^/]+)/instances/([^/]+)(?:/appProfiles/([^/]+))?/tables/([^/?#]+)$)");
  std::smatch match;
  if (definition.uris.size() != 1 || !std::regex_match(definition.uris.front(), match, pattern)) {
    throw ApiError::Invalid("A Bigtable external table requires one Bigtable table URI");
  }
  return {
      .project = match.str(1),
      .instance = match.str(2),
      .profile = match.str(3),
      .table = match.str(4),
  };
}

void ValidateBigtableExternal(const GcsExternalData& definition) {
  ParseBigtableSource(definition);
  if (definition.autodetect || definition.enable_list_inference || definition.skip_leading_rows ||
      definition.field_delimiter || definition.quote || definition.allow_quoted_newlines ||
      definition.allow_jagged_rows || definition.null_marker || definition.null_markers ||
      definition.preserve_ascii_control_characters || definition.ignore_unknown_values ||
      definition.compression || definition.max_bad_records || definition.encoding ||
      definition.hive_partitioning || definition.time_zone || definition.date_format ||
      definition.datetime_format || definition.time_format || definition.timestamp_format) {
    throw ApiError::Unsupported("GCS read options on Bigtable external tables");
  }
  if (definition.bigtable_options) {
    std::set<std::string> names{"rowkey"};
    const auto& families = definition.bigtable_options->column_families;
    const auto duplicate = std::ranges::find_if(families, [&names](const BigtableFamily& family) {
      return !names.insert(ToLowerAscii(family.family_id)).second;
    });
    if (duplicate != families.end()) {
      throw ApiError::Invalid("Duplicate Bigtable family: " + duplicate->family_id);
    }
  }
}

}  // namespace bigquery_emulator_duckdb
