#include "src/load.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/gcs.h"
#include "src/schema_sql.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {

using nlohmann::json;

std::vector<std::string> StageLoadSources(const json& config, const std::string& format,
                                          GcsClient& gcs, TemporaryFiles& downloads) {
  const json uris = config.value("sourceUris", json::array());
  if (!uris.is_array() || uris.empty()) throw ApiError::Invalid("sourceUris is required");
  std::vector<std::string> sources;
  for (const json& item : uris) {
    if (!item.is_string()) throw ApiError::Invalid("Invalid source URI");
    const std::string uri = item.get<std::string>();
    if (uri.starts_with("gs://")) {
      const auto matches = gcs.Expand(uri);
      sources.insert(sources.end(), matches.begin(), matches.end());
    } else {
      sources.push_back(uri);
    }
  }
  std::vector<std::string> paths;
  for (const std::string& uri : sources) {
    std::string path;
    if (uri.starts_with("gs://")) {
      path = downloads.Create();
      gcs.Download(uri, std::filesystem::path(path));
    } else if (uri.starts_with("file://")) {
      path = uri.substr(7);
    } else if (uri.find("://") == std::string::npos) {
      path = uri;
    } else {
      throw ApiError::Invalid("Unsupported source URI: " + uri);
    }
    if (path.empty() || !std::filesystem::is_regular_file(path)) {
      throw ApiError::Invalid("Source file does not exist: " + uri);
    }
    // Uploads and downloads lose their original suffix. DuckDB selects gzip by suffix,
    // so inspect the bytes and stage gzip inputs under a name its readers recognize.
    if (format != "PARQUET") {
      std::ifstream input(path, std::ios::binary);
      const bool gzip = input.get() == 0x1f && input.get() == 0x8b;
      if (gzip) {
        const std::string compressed = downloads.Create(".gz");
        if (uri.starts_with("gs://")) {
          std::filesystem::rename(path, compressed);
        } else {
          std::filesystem::copy_file(path, compressed,
                                     std::filesystem::copy_options::overwrite_existing);
        }
        path = compressed;
      }
    }
    paths.push_back(path);
  }
  return paths;
}

namespace {

bool HasBigNumeric(const std::vector<FieldSchema>& fields) {
  return std::ranges::any_of(fields, [](const FieldSchema& field) {
    return field.type == FieldType::kBigNumeric || HasBigNumeric(field.fields);
  });
}

}  // namespace

std::string LoadQuery(const std::string& format, const std::vector<std::string>& paths,
                      const json& config, const std::vector<FieldSchema>& schema) {
  std::string files;
  for (const std::string& path : paths) {
    files += (files.empty() ? "" : ", ") + QuoteLiteral(path);
  }
  files = "[" + files + "]";
  std::string sql;
  if (format == "CSV") {
    sql = "SELECT * FROM read_csv(" + files +
          ", header=false, skip=" + std::to_string(config.value("skipLeadingRows", 0)) +
          ", delim=" + QuoteLiteral(config.value("fieldDelimiter", ","));
    if (!schema.empty()) {
      sql += ", auto_detect=false";
      std::string columns;
      // read_csv() would truncate a BIGNUM, so a BIGNUMERIC is read as text and converted.
      std::string conversions;
      for (const FieldSchema& field : schema) {
        const bool big = field.type == FieldType::kBigNumeric;
        columns += (columns.empty() ? "" : ", ") + QuoteLiteral(field.name) + ": " +
                   QuoteLiteral(big ? "VARCHAR" : ToDuckDbType(field));
        conversions +=
            (conversions.empty() ? "" : ", ") +
            (big ? std::format("CAST(bq_bignumeric_from_string({0}, false) AS {1}) AS {0}",
                               QuoteIdentifier(field.name), ToDuckDbType(field))
                 : QuoteIdentifier(field.name));
      }
      sql += ", columns={" + columns + "})";
      return std::format("SELECT {} FROM ({}) AS source", conversions, sql);
    }
    sql += ")";
  } else if (HasBigNumeric(schema)) {
    // JSON numbers would be read as DOUBLE.
    throw ApiError::Invalid("Loading BIGNUMERIC from " + format + " is not supported");
  } else if (format == "NEWLINE_DELIMITED_JSON") {
    sql = std::format("SELECT * FROM read_json({}, format='newline_delimited')", files);
  } else {
    sql = std::format("SELECT * FROM read_parquet({})", files);
  }
  if (!schema.empty() && format != "CSV") {
    std::string columns;
    for (const FieldSchema& field : schema) {
      if (!columns.empty()) columns += ", ";
      columns +=
          std::format("CAST({0} AS {1}) AS {0}", QuoteIdentifier(field.name), ToDuckDbType(field));
    }
    sql = std::format("SELECT {} FROM ({}) AS source", columns, sql);
  }
  return sql;
}

}  // namespace bigquery_emulator_duckdb
