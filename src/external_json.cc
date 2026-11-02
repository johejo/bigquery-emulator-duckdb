#include "src/external_json.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/field_schema.h"
#include "src/file_read.h"
#include "src/schema_detect.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

std::string Folded(std::string_view name) {
  std::string folded;
  std::ranges::transform(name, std::back_inserter(folded),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return folded;
}

void ValidateObject(json& object, const std::vector<FieldSchema>& schema, bool converts);

// Reject unimplemented representations before DuckDB can coerce them, for example rounding a
// fractional INT64. External read observations are retained in
// external_table_json.api.answers.json, and those of autodetected tables, which `converts` the
// values of, in external_table_json_autodetect.api.answers.json: a string converts to the type
// DetectValue finds in it, or an INT64 to a FLOAT64, and a number or a BOOL to a STRING.
void ValidateValue(json& value, const FieldSchema& field, bool converts, bool element = false) {
  if (value.is_null()) {
    if (element || field.mode == FieldMode::kRequired) {
      throw ApiError::Invalid("Only optional fields can be set to NULL. Field: " + field.name +
                              "; Value: NULL");
    }
    if (field.mode == FieldMode::kRepeated) {
      value = json::array();
    }
    return;
  }
  if (field.mode == FieldMode::kRepeated && !element) {
    if (!value.is_array()) {
      throw ApiError::Invalid(
          "Repeated field must be imported as a JSON array. Field: " + field.name + ".");
    }
    for (auto& item : value) {
      ValidateValue(item, field, converts, true);
    }
    return;
  }
  if (element && value.is_array()) {
    throw ApiError::Invalid("Nested arrays not allowed.");
  }
  // A converted string, or none.
  const auto detected = [&]() -> std::optional<DetectedValue> {
    if (!converts || !value.is_string()) {
      return std::nullopt;
    }
    return DetectValue(value.get_ref<const std::string&>());
  }();
  switch (field.type) {
    case FieldType::kRecord:
      if (!value.is_object()) {
        throw ApiError::Invalid("Flat value specified for record field. Field: " + field.name +
                                "; Value: " + value.dump());
      }
      ValidateObject(value, field.fields, converts);
      return;
    case FieldType::kString:
    case FieldType::kBytes:
      if (value.is_string()) {
        return;
      }
      // StageJsonNumerics writes the numbers of STRING fields as their text.
      if (converts && field.type == FieldType::kString && value.is_boolean()) {
        value = value.dump();
        return;
      }
      break;
    case FieldType::kBoolean:
      if (value.is_boolean()) {
        return;
      }
      if (value.is_string()) {
        const auto text = Folded(value.get_ref<const std::string&>());
        if (text == "true" || text == "t" || text == "yes" || text == "y" || text == "1") {
          value = true;
          return;
        }
        if (text == "false" || text == "f" || text == "no" || text == "n" || text == "0") {
          value = false;
          return;
        }
      }
      break;
    case FieldType::kFloat:
      if (value.is_number()) {
        return;
      }
      if (detected &&
          (detected->kind == DetectedKind::kInteger || detected->kind == DetectedKind::kFloat)) {
        value = detected->text;
        return;
      }
      break;
    case FieldType::kInteger:
      if (value.is_number_unsigned()) {
        if (value.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
          throw ApiError::Invalid("JSON INT64 out of range: " + field.name);
        }
        // Keep native INT64 tokens exact through the typed reader, like decimal strings.
        value = std::to_string(value.get<uint64_t>());
        return;
      }
      if (value.is_number_integer()) {
        value = std::to_string(value.get<int64_t>());
        return;
      }
      if (value.is_number_float()) {
        throw ApiError::Invalid("Could not convert value 'number_value: \t \"" + value.dump() +
                                "\"' to integer. Field: " + field.name +
                                "; Value: " + value.dump());
      }
      if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        int64_t integer = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), integer);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) {
          return;
        }
      }
      if (detected && detected->kind == DetectedKind::kInteger) {
        value = detected->text;
        return;
      }
      break;
    case FieldType::kNumeric:
      // StageJsonNumerics preserves original numeric tokens as strings before DOM parsing.
      if (value.is_string()) {
        static const std::regex decimal(R"(-?[0-9]{1,29}(\.[0-9]{1,9})?)");
        if (std::regex_match(value.get_ref<const std::string&>(), decimal)) {
          return;
        }
      }
      break;
    case FieldType::kDate:
    case FieldType::kTime:
    case FieldType::kTimestamp:
      if (detected && KindType(detected->kind) == field.type) {
        value = detected->text;
        return;
      }
      if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        // Only canonical dates, times and UTC timestamps are accepted. The typed reader
        // validates calendar dates; these guards prevent out-of-range years and time rounding.
        static const std::string date = "[0-9]{4}-[0-9]{2}-[0-9]{2}";
        static const std::string time =
            R"(([01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9](\.[0-9]{1,6})?)";
        static const std::regex date_pattern(date);
        static const std::regex time_pattern(time);
        static const std::regex timestamp_pattern(date + "T" + time + "Z");
        const auto& pattern = field.type == FieldType::kDate   ? date_pattern
                              : field.type == FieldType::kTime ? time_pattern
                                                               : timestamp_pattern;
        if (std::regex_match(text, pattern) &&
            (field.type == FieldType::kTime || !text.starts_with("0000"))) {
          return;
        }
      }
      break;
    default:
      break;
  }
  throw ApiError::Unsupported("JSON external representation for " + field.name);
}

void ValidateObject(json& object, const std::vector<FieldSchema>& schema, bool converts) {
  if (!object.is_object()) {
    throw ApiError::Invalid("Expected a JSON object");
  }
  json canonical = json::object();
  for (const auto& [name, value] : object.items()) {
    const auto field = std::ranges::find_if(schema, [&name](const FieldSchema& declared) {
      return Folded(declared.name) == Folded(name);
    });
    if (field == schema.end()) {
      throw ApiError::Invalid("No such field: " + name + ".");
    }
    if (canonical.contains(field->name)) {
      throw ApiError::Unsupported("case variant duplicate JSON external field: " + name);
    }
    ValidateValue(value, *field, converts);
    canonical[field->name] = std::move(value);
  }
  for (const auto& field : schema) {
    if (!canonical.contains(field.name)) {
      if (field.mode == FieldMode::kRequired) {
        throw ApiError::Invalid("Missing required field: " + field.name);
      }
      ValidateValue(canonical[field.name], field, converts);
    }
  }
  object = std::move(canonical);
}

FileSource StageSource(const FileSource& source, const std::vector<FieldSchema>& schema,
                       TemporaryFiles& files, bool converts) {
  const std::string contents = ReadMaybeGzip(source.path);
  std::string rewritten;
  for (std::size_t start = 0; start < contents.size();) {
    const auto newline = contents.find('\n', start);
    const auto end = newline == std::string::npos ? contents.size() : newline;
    const std::string_view line(contents.data() + start, end - start);
    if (line.find_first_not_of(" \t\r") != std::string_view::npos) {
      try {
        std::vector<std::set<std::string>> keys;
        const auto unique_keys = [&keys](int /*depth*/, json::parse_event_t event, json& parsed) {
          using Event = json::parse_event_t;
          if (event == Event::object_start) {
            keys.emplace_back();
          } else if (event == Event::object_end) {
            keys.pop_back();
          } else if (event == Event::key && !keys.back().insert(parsed.get<std::string>()).second) {
            throw ApiError::Invalid("Multiple definition of field: " + parsed.get<std::string>() +
                                    ".");
          }
          return true;
        };
        auto object = json::parse(line, unique_keys, false);
        if (object.is_discarded()) {
          throw ApiError::Invalid("Invalid JSON in " + source.uri);
        }
        ValidateObject(object, schema, converts);
        rewritten += object.dump() + '\n';
      } catch (const ApiError& error) {
        if (std::string_view(error.what()).starts_with(kUnsupported)) {
          throw;
        }
        throw error.WithMessage(
            std::format("JSON parsing error in row starting at position {}: {} File: {}", start,
                        error.what(), source.uri));
      }
    } else if (line.find_first_of(" \t") != std::string_view::npos) {
      // BigQuery reads an empty line, but not one of spaces.
      throw ApiError::Invalid(std::format(
          "JSON parsing error in row starting at position {}: Unexpected token File: {}", start,
          source.uri));
    }
    start = end + 1;
  }
  return {.uri = source.uri, .path = files.Write(rewritten)};
}

}  // namespace

std::vector<FileSource> StageExternalJson(const std::vector<FileSource>& sources,
                                          const std::vector<FieldSchema>& schema,
                                          TemporaryFiles& files, bool autodetected) {
  const auto numeric = StageJsonNumerics(sources, schema, files, autodetected);
  std::vector<FileSource> staged;
  std::ranges::transform(numeric, std::back_inserter(staged), [&](const FileSource& source) {
    return StageSource(source, schema, files, autodetected);
  });
  return staged;
}

}  // namespace bigquery_emulator_duckdb
