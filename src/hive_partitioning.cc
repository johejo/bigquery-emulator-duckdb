#include "src/hive_partitioning.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/str_join.h"
#include "src/api_error.h"
#include "src/field_schema.h"
#include "src/schema_detect.h"
#include "src/table_metadata.h"

// The rules below follow BigQuery's answers in
// tests/e2e/scenarios/testdata/external_table_hive.api.answers.json, which
// third_party/bigquery_docs/hive-partitioned-queries.md describes only in outline.

namespace bigquery_emulator_duckdb {
namespace {

constexpr std::string_view kDefaultPartition = "__HIVE_DEFAULT_PARTITION__";
constexpr size_t kMaxKeys = 10;

// The keys a source's directory encodes under the prefix, in order.
struct HivePath {
  // The directory under the prefix, without slashes around it.
  std::string directory;
  std::vector<std::pair<std::string, std::string>> keys;
};

// The prefix the partition directories follow, without a trailing slash or a CUSTOM schema.
std::string BasePrefix(const HivePartitioning& partitioning) {
  std::string prefix = partitioning.source_uri_prefix;
  if (const size_t schema = prefix.find("/{"); schema != std::string::npos) {
    prefix.resize(schema);
  }
  while (prefix.ends_with('/')) {
    prefix.pop_back();
  }
  return prefix;
}

// gs://bucket/path as BigQuery names it in errors, /bigstore/bucket/path.
std::string Bigstore(std::string_view uri) {
  uri.remove_prefix(std::min(uri.size(), std::string_view("gs://").size()));
  return "/bigstore/" + std::string(uri);
}

// The keys of `uri`: each directory under the prefix of the form name=value, with a name and a
// value; BigQuery passes over other directories.
HivePath ParsePath(const HivePartitioning& partitioning, const std::string& uri) {
  const std::string prefix = BasePrefix(partitioning);
  const std::string directory = uri.substr(0, uri.rfind('/') + 1);
  if (!directory.starts_with(prefix)) {
    throw ApiError::InvalidQuery(
        std::format("Directory URI {} does not contain path to table ({}) as a prefix, which is a "
                    "requirement.",
                    Bigstore(directory), Bigstore(prefix)));
  }
  HivePath path;
  std::string_view rest = std::string_view(directory).substr(prefix.size());
  while (rest.starts_with('/')) {
    rest.remove_prefix(1);
  }
  while (rest.ends_with('/')) {
    rest.remove_suffix(1);
  }
  path.directory = rest;
  while (!rest.empty()) {
    const size_t slash = rest.find('/');
    const std::string_view segment = rest.substr(0, slash);
    rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
    const size_t equals = segment.find('=');
    if (equals != std::string_view::npos && equals > 0 && equals + 1 < segment.size()) {
      path.keys.emplace_back(segment.substr(0, equals), segment.substr(equals + 1));
    }
  }
  return path;
}

std::vector<std::string> Names(const std::vector<std::pair<std::string, std::string>>& keys) {
  std::vector<std::string> names;
  std::ranges::transform(keys, std::back_inserter(names),
                         [](const auto& key) { return key.first; });
  return names;
}

// Checks that `path` has the keys `expected`, in order.
void CheckKeys(const HivePath& path, const std::vector<std::string>& expected) {
  const std::vector<std::string> names = Names(path.keys);
  if (names.size() != expected.size()) {
    throw ApiError::InvalidQuery(std::format(
        "Partition keys should be invariant from table creation across all partitions, with the "
        "number of partition keys held constant with invariant names.  Expected {} partition "
        "keys ([{}]), but {} ([{}]) were encountered for partition {}.",
        expected.size(), absl::StrJoin(expected, ", "), names.size(), absl::StrJoin(names, ", "),
        path.directory));
  }
  for (size_t i = 0; i < names.size(); ++i) {
    if (names.at(i) != expected.at(i)) {
      throw ApiError::InvalidQuery(std::format(
          "Partition keys should be invariant from table creation across all partitions.  "
          "Encountered partition key {} when partition key {} was expected at position {}.  To "
          "query this data, either disable requesting hive partition inference, or make "
          "directory structure consistent.  Expected partition schema: [{}], encountered "
          "partition schema: [{}].",
          names.at(i), expected.at(i), i, absl::StrJoin(expected, ", "),
          absl::StrJoin(names, ", ")));
    }
  }
}

void NoKeys(const HivePartitioning& partitioning, std::string_view table_id) {
  throw ApiError::Invalid(std::format(
      "Error while reading table: {0}, error message: Table {0} requested hive partitioning, but "
      "no partition keys were detected.  This is a sign of misconfiguration. Please ensure the "
      "source uri prefix provided is the prefix immediately before partition encoding begins. "
      "For instance, 'gs://bucket/table/key1=today/key2=5/foo.txt' would have "
      "'gs://bucket/table' as a valid source uri prefix.  '{1}' was provided as the source uri "
      "prefix.",
      table_id, partitioning.source_uri_prefix));
}

std::string_view TypeName(FieldType type) {
  switch (type) {
    case FieldType::kInteger:
      return "TYPE_INT64";
    case FieldType::kDate:
      return "TYPE_DATE";
    case FieldType::kTimestamp:
      return "TYPE_TIMESTAMP";
    default:
      return "TYPE_STRING";
  }
}

// The text of `value` that a CAST to `key`'s type reads, or none for one it does not hold.
std::optional<std::string> KeyValue(const FieldSchema& key, const std::string& value) {
  if (key.type == FieldType::kString) {
    return value;
  }
  DetectedValue detected = DetectValue(value);
  if (KindType(detected.kind) != key.type) {
    return std::nullopt;
  }
  return std::move(detected.text);
}

// The type AUTO detects for `value`.
FieldType AutoType(const std::string& value) {
  const FieldType type = KindType(DetectValue(value).kind);
  return type == FieldType::kInteger || type == FieldType::kDate || type == FieldType::kTimestamp
             ? type
             : FieldType::kString;
}

// The paths of `uris`, checked in BigQuery's order: the alphabetically last, which gives the keys
// BigQuery expects of the others, first, then all of them alphabetically.
std::vector<HivePath> Paths(const HivePartitioning& partitioning, std::vector<std::string> uris,
                            std::string_view table_id) {
  if (uris.empty()) {
    return {};
  }
  std::ranges::sort(uris);
  std::vector<HivePath> paths{ParsePath(partitioning, uris.back())};
  if (paths.front().keys.empty()) {
    NoKeys(partitioning, table_id);
  }
  for (const std::string& uri : uris) {
    HivePath& path = paths.emplace_back(ParsePath(partitioning, uri));
    if (path.keys.size() > kMaxKeys) {
      throw ApiError::InvalidQuery(std::format(
          "Detected {} partition keys. At most {} are supported. Partition key names are: [{}]. "
          "Directory examined: {}.",
          path.keys.size(), kMaxKeys, absl::StrJoin(Names(path.keys), ", "), path.directory));
    }
    if (path.keys.empty() && path.directory.empty()) {
      NoKeys(partitioning, table_id);
    }
  }
  return paths;
}

}  // namespace

void CheckHiveKeyType(FieldType type, std::string_view name) {
  if (type != FieldType::kString && type != FieldType::kTimestamp && type != FieldType::kInteger &&
      type != FieldType::kDate) {
    throw ApiError::Invalid(std::format(
        "'{}' is not a valid partition key type.  Valid types are [STRING, TIMESTAMP, INTEGER, "
        "INT64, DATE]",
        name));
  }
}

std::vector<FieldSchema> CustomPrefixKeys(const std::string& prefix) {
  std::vector<FieldSchema> keys;
  for (size_t open = prefix.find("/{"); open != std::string::npos;
       open = prefix.find("/{", open + 1)) {
    const size_t close = prefix.find('}', open);
    const size_t colon = prefix.find(':', open);
    if (close == std::string::npos || colon == std::string::npos || colon > close) {
      return {};
    }
    const std::string name = prefix.substr(open + 2, colon - open - 2);
    const std::string type = prefix.substr(colon + 1, close - colon - 1);
    const FieldType parsed =
        type == "INT64" ? FieldType::kInteger : ParseFieldType(type).value_or(FieldType::kRecord);
    CheckHiveKeyType(parsed, type);
    keys.push_back({.name = name, .type = parsed});
  }
  return keys;
}

std::vector<FieldSchema> DetectHiveKeys(const HivePartitioning& partitioning,
                                        const std::vector<FieldSchema>& declared,
                                        const std::vector<std::string>& uris,
                                        std::string_view table_id) {
  const std::vector<HivePath> paths = Paths(partitioning, uris, table_id);
  if (paths.empty()) {
    return {};
  }
  const bool custom = partitioning.mode == "CUSTOM";
  std::vector<FieldSchema> keys = declared;
  if (!custom) {
    const auto& reference = paths.front().keys;
    keys.clear();
    std::ranges::transform(reference, std::back_inserter(keys),
                           [](const auto& key) { return FieldSchema{.name = key.first}; });
  }
  std::vector<std::string> expected;
  std::ranges::transform(keys, std::back_inserter(expected), &FieldSchema::name);
  // The types each key's values show, absent while it has had only the default partition.
  std::vector<std::optional<FieldType>> types(keys.size());
  // The reference comes again among the others.
  for (const HivePath& path : std::ranges::drop_view(paths, 1)) {
    CheckKeys(path, expected);
    for (size_t i = 0; i < keys.size(); ++i) {
      const std::string& value = path.keys.at(i).second;
      if (value == kDefaultPartition) {
        continue;
      }
      if (custom) {
        if (!KeyValue(keys.at(i), value)) {
          std::vector<std::string> schema;
          std::ranges::transform(keys, std::back_inserter(schema), [](const FieldSchema& key) {
            return key.name + ":" + std::string(TypeName(key.type));
          });
          throw ApiError::InvalidQuery(std::format(
              "Partition key '{}' was provided with type {}, but '{}' cannot be parsed as {}.  "
              "Full schema provided: [{}].",
              keys.at(i).name, TypeName(keys.at(i).type), value, TypeName(keys.at(i).type),
              absl::StrJoin(schema, ", ")));
        }
        continue;
      }
      const FieldType type = partitioning.mode == "STRINGS" ? FieldType::kString : AutoType(value);
      std::optional<FieldType>& seen = types.at(i);
      seen = seen.has_value() && seen.value() != type ? FieldType::kString : type;
    }
  }
  if (!custom) {
    for (size_t i = 0; i < keys.size(); ++i) {
      keys.at(i).type = types.at(i).value_or(FieldType::kString);
    }
  }
  for (const FieldSchema& key : keys) {
    CheckDetectedFieldName(key.name);
  }
  return keys;
}

std::vector<std::optional<std::string>> HiveValues(const HivePartitioning& partitioning,
                                                   const std::vector<FieldSchema>& keys,
                                                   const std::string& uri,
                                                   std::string_view table_id) {
  const HivePath path = ParsePath(partitioning, uri);
  if (path.keys.empty() && path.directory.empty()) {
    NoKeys(partitioning, table_id);
  }
  std::vector<std::string> expected;
  std::ranges::transform(keys, std::back_inserter(expected), &FieldSchema::name);
  CheckKeys(path, expected);
  std::vector<std::optional<std::string>> values;
  for (size_t i = 0; i < keys.size(); ++i) {
    const std::string& value = path.keys.at(i).second;
    if (value == kDefaultPartition) {
      values.emplace_back();
      continue;
    }
    std::optional<std::string> text = KeyValue(keys.at(i), value);
    if (!text) {
      throw ApiError::Invalid(std::format("Partition key '{}' cannot be parsed as {} from '{}'",
                                          keys.at(i).name, TypeName(keys.at(i).type), value));
    }
    values.push_back(std::move(text));
  }
  return values;
}

}  // namespace bigquery_emulator_duckdb
