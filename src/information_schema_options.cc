#include "src/information_schema_options.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "absl/strings/str_join.h"
#include "googlesql/public/strings.h"
#include "src/information_schema_rows.h"

namespace bigquery_emulator_duckdb::information_schema {

std::string StringOptionValue(const std::string& value) {
  return googlesql::ToDoubleQuotedStringLiteral(value);
}

// The options that metadata of a dataset, a table or a view sets, which are common to both.
std::vector<Option> CommonOptions(const std::string& description, const std::string& friendly_name,
                                  const std::map<std::string, std::string>& labels,
                                  const std::string& default_rounding_mode,
                                  const std::vector<std::string>& label_order) {
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
    const auto append = [&](const std::string& key) {
      if (const auto it = labels.find(key); it != labels.end()) {
        entries.push_back("STRUCT(" + StringOptionValue(it->first) + ", " +
                          StringOptionValue(it->second) + ")");
      }
    };
    if (label_order.empty()) {
      for (const auto& label : labels) {
        append(label.first);
      }
    } else {
      for (const std::string& key : label_order) {
        append(key);
      }
    }
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

}  // namespace bigquery_emulator_duckdb::information_schema
