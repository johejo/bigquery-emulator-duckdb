#include "src/external_json.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
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

#include "absl/status/status.h"
#include "googlesql/public/numeric_value.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/bignumeric.h"
#include "src/external_values.h"
#include "src/field_schema.h"
#include "src/file_read.h"
#include "src/file_read_options.h"
#include "src/schema_detect.h"
#include "src/temporary_files.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

std::string Folded(std::string_view name) {
  std::string folded;
  std::ranges::transform(name, std::back_inserter(folded),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return folded;
}

void ValidateObject(json& object, const std::vector<FieldSchema>& schema, bool converts,
                    const DatetimeFormats& formats);

// BigQuery's message for the line `line`, which is not a JSON object, as its answers in
// external_table_json_types.api.answers.json, external_table_bad_records.api.answers.json and
// external_table_types_open.api.answers.json show: an array, a string, a line that ends where a
// key of the row is expected and a bracket that closes the row are named, and other malformed
// lines take its most common message. Other JSON values are unsupported.
std::string_view NotObjectMessage(std::string_view line) {
  const std::size_t first = line.find_first_not_of(" \t\r");
  if (line.at(first) == '[') {
    return "Start of array encountered without start of object.";
  }
  if (line.at(first) == '"' && json::accept(line)) {
    return "Value encountered without start of object.";
  }
  try {
    (void)json::parse(line);
  } catch (const json::parse_error& error) {
    // The containers open where the parser stopped.
    const std::size_t at = std::min<std::size_t>(error.byte - 1, line.size());
    std::vector<char> open;
    bool in_string = false;
    for (std::size_t i = 0; i < at; ++i) {
      const char c = line.at(i);
      if (in_string) {
        if (c == '\\') {
          ++i;
        } else if (c == '"') {
          in_string = false;
        }
      } else if (c == '"') {
        in_string = true;
      } else if (c == '{' || c == '[') {
        open.push_back(c);
      } else if ((c == '}' || c == ']') && !open.empty()) {
        open.pop_back();
      }
    }
    const bool row = open.size() == 1 && open.front() == '{';
    if (row &&
        std::string_view(error.what()).find("while parsing object key - unexpected end of input") !=
            std::string_view::npos) {
      return "Expected key";
    }
    if (row && at < line.size() && line.at(at) == ']') {
      return "Expected , or } after key:value pair";
    }
    return "Parser terminated before end of string";
  }
  throw ApiError::Unsupported("reading a JSON row that is a number, a BOOL or null");
}

// `line` without the comma after the last member of its object, which BigQuery accepts, as its
// answers in external_table_types_open.api.answers.json show, or none when it has none.
std::optional<std::string> WithoutTrailingComma(std::string_view line) {
  const std::size_t close = line.find_last_not_of(" \t\r");
  if (close == std::string_view::npos || close == 0 || line.at(close) != '}') {
    return std::nullopt;
  }
  const std::size_t comma = line.find_last_not_of(" \t\r", close - 1);
  if (comma == std::string_view::npos || comma == 0 || line.at(comma) != ',') {
    return std::nullopt;
  }
  // Whether BigQuery reads {,} is not known.
  const std::size_t before = line.find_last_not_of(" \t\r", comma - 1);
  if (before == std::string_view::npos || line.at(before) == '{' || line.at(before) == ',') {
    return std::nullopt;
  }
  std::string without(line);
  without.erase(comma, 1);
  return json::accept(without) ? std::optional(std::move(without)) : std::nullopt;
}

// Whether `formats` set how a DATE, TIME or TIMESTAMP `type` value is parsed.
bool FormatsApply(FieldType type, const DatetimeFormats& formats) {
  switch (type) {
    case FieldType::kDate:
      return !formats.date.empty();
    case FieldType::kTime:
      return !formats.time.empty();
    case FieldType::kTimestamp:
      return !formats.timestamp.empty() || !formats.time_zone.empty();
    default:
      return false;
  }
}

// `value` as BigQuery's messages show it, a BOOL as 1 or 0.
std::string Shown(const json& value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_boolean()) {
    return value.get<bool>() ? "1" : "0";
  }
  return value.dump();
}

// Converts `value`, of a DATETIME, BIGNUMERIC or GEOGRAPHY `field`, to the text the reader reads,
// as ExternalValue takes it from a string, or a number for BIGNUMERIC, which StageSource quotes.
// As BigQuery's answers in external_table_json_types.api.answers.json show, a string that does
// not convert fails as a query error, and other values make their record unparsable.
void ConvertValue(json& value, const FieldSchema& field, const DatetimeFormats& formats) {
  if (value.is_object()) {
    throw ApiError::Invalid("JSON object specified for non-record field: " + field.name);
  }
  if (!value.is_string()) {
    if (value.is_array()) {
      throw ApiError::Unsupported("JSON external representation for " + field.name);
    }
    const std::string shown = Shown(value);
    throw ApiError::Invalid(
        field.type == FieldType::kDatetime
            ? std::format("Could not convert non-string JSON value to DATETIME type. Field: {}; "
                          "Value: {}",
                          field.name, shown)
        : field.type == FieldType::kBigNumeric
            ? std::format(
                  "Could not convert value to numeric/bignumeric type. Field: {}; Value: {}",
                  field.name, shown)
            : std::format("Could not convert JSON value to geography. Field: {}; Value: {}",
                          field.name, shown));
  }
  const auto& text = value.get_ref<const std::string&>();
  const auto converted = ExternalValue(field.type, text, formats);
  if (!converted.ok()) {
    const std::string message(converted.status().message());
    if (absl::IsUnimplemented(converted.status())) {
      throw ApiError::Invalid(message);
    }
    throw ApiError::InvalidQuery(std::format(
        "{}{} Field: {}; Value: {}",
        field.type == FieldType::kGeography ? "Could not convert JSON value to geography: " : "",
        message, field.name, text));
  }
  // The reader reads a BIGNUMERIC from the text of its units.
  value = field.type == FieldType::kBigNumeric
              ? BigNumericUnits(*googlesql::BigNumericValue::FromStringStrict(*converted))
              : *converted;
}

// Reject unimplemented representations before DuckDB can coerce them, for example rounding a
// fractional INT64. External read observations are retained in
// external_table_json.api.answers.json, and those of autodetected tables, which `converts` the
// values of, in external_table_json_autodetect.api.answers.json: a string converts to the type
// DetectValue finds in it, or an INT64 to a FLOAT64, and a number or a BOOL to a STRING.
void ValidateValue(json& value, const FieldSchema& field, bool converts,
                   const DatetimeFormats& formats, bool element = false) {
  // A JSON field holds any JSON value, null included, which the reader parses from its text.
  if (field.type == FieldType::kJson && (element || field.mode != FieldMode::kRepeated)) {
    value = value.dump();
    return;
  }
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
      ValidateValue(item, field, converts, formats, true);
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
      ValidateObject(value, field.fields, converts, formats);
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
    case FieldType::kDatetime:
    case FieldType::kBigNumeric:
    case FieldType::kGeography:
      ConvertValue(value, field, formats);
      return;
    case FieldType::kDate:
    case FieldType::kTime:
    case FieldType::kTimestamp:
      // A TIMESTAMP of a table with a given schema takes the forms ExternalValue takes.
      if (value.is_string() && (FormatsApply(field.type, formats) ||
                                (field.type == FieldType::kTimestamp && !converts))) {
        const auto& text = value.get_ref<const std::string&>();
        const bool zoned = field.type == FieldType::kTimestamp && formats.timestamp.empty();
        if (zoned && IsEpoch(text)) {
          throw ApiError::Unsupported("JSON external representation for " + field.name);
        }
        const auto converted = ExternalValue(field.type, text, formats);
        if (!converted.ok()) {
          if (absl::IsUnimplemented(converted.status())) {
            throw ApiError::Unsupported("JSON external representation for " + field.name);
          }
          // As BigQuery's answers in external_table_datetime_formats.api.answers.json and
          // external_table_types_followups.api.answers.json show, a value that does not parse
          // fails as a query error, with CAST's message where a format applies.
          const std::string message(converted.status().message());
          throw ApiError::InvalidQuery(
              zoned ? std::format("{} Field: {}; Value: {}", message, field.name, text) : message);
        }
        value = *converted;
        return;
      }
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

void ValidateObject(json& object, const std::vector<FieldSchema>& schema, bool converts,
                    const DatetimeFormats& formats) {
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
    ValidateValue(value, *field, converts, formats);
    canonical[field->name] = std::move(value);
  }
  for (const auto& field : schema) {
    if (!canonical.contains(field.name)) {
      if (field.mode == FieldMode::kRequired) {
        throw ApiError::Invalid("Missing required field: " + field.name);
      }
      // A JSON field that the object leaves out is NULL rather than a JSON null.
      if (field.type != FieldType::kJson || field.mode == FieldMode::kRepeated) {
        ValidateValue(canonical[field.name], field, converts, formats);
      }
    }
  }
  // A RECORD of no fields reads as DuckDB keeps a STRUCT<>.
  if (schema.empty()) {
    canonical[std::string(kEmptyStruct)] = true;
  }
  object = std::move(canonical);
}

// Adds the record of `source` that starts at `start`, which BigQuery rejects with `error`, to
// `bad`, or throws BigQuery's error when `bad` is null or the record exceeds its limit. An
// invalidQuery error, of a value that does not convert, keeps its message.
void Bad(const ApiError& error, std::size_t start, const FileSource& source, BadRecords* bad) {
  const bool query = error.reason() == "invalidQuery";
  const std::string message =
      query ? error.what()
            : std::format("JSON parsing error in row starting at position {}: {}", start,
                          error.what());
  if (bad == nullptr) {
    throw query ? error : ApiError::Invalid(message + " File: " + source.uri);
  }
  if (!bad->Skip({.number = bad->records, .uri = source.uri, .message = message, .query = query})) {
    throw ApiError::Invalid(std::format(
        "JSON table encountered too many errors, giving up. Rows: {}; errors: {}. Please look "
        "into the errors[] collection for more details. File: {}",
        bad->records, bad->found.size(), source.uri));
  }
}

// The field of `fields` that `name` names, in any case, or none.
const FieldSchema* FindField(const std::vector<FieldSchema>& fields, const std::string& name) {
  const auto field = std::ranges::find_if(fields, [&name](const FieldSchema& declared) {
    return Folded(declared.name) == Folded(name);
  });
  return field == fields.end() ? nullptr : &*field;
}

// Rejects as unsupported a field of `value`, a value of the RECORD `fields` or an array of them,
// that `fields` lacks: how BigQuery reads such a nested field with a given schema is not known.
void CheckNestedFields(const json& value, const std::vector<FieldSchema>& fields) {
  if (value.is_array()) {
    for (const json& item : value) {
      CheckNestedFields(item, fields);
    }
    return;
  }
  if (!value.is_object()) {
    return;
  }
  for (const auto& [name, item] : value.items()) {
    const FieldSchema* field = FindField(fields, name);
    if (field == nullptr) {
      throw ApiError::Unsupported(
          "reading a nested JSON field that the given schema of an autodetected external table "
          "lacks: " +
          name);
    }
    if (field->type == FieldType::kRecord) {
      CheckNestedFields(item, field->fields);
    }
  }
}

// Leaves out of `object`, a row of a JSON table with autodetect and a given `schema`, the
// top-level fields `schema` lacks, as BigQuery's reads do in its answers in
// external_table_json_autodetect.api.answers.json.
void LeaveOutUnknownFields(json& object, const std::vector<FieldSchema>& schema) {
  for (auto it = object.begin(); it != object.end();) {
    const FieldSchema* field = FindField(schema, it.key());
    if (field == nullptr) {
      it = object.erase(it);
      continue;
    }
    if (field->type == FieldType::kRecord) {
      CheckNestedFields(it.value(), field->fields);
    }
    ++it;
  }
}

FileSource StageSource(const FileSource& source, const std::vector<FieldSchema>& schema,
                       TemporaryFiles& files, bool converts, bool given_schema,
                       const DatetimeFormats& formats, BadRecords* bad) {
  const std::string contents = ReadMaybeGzip(source.path);
  std::string rewritten;
  // Where the next row starts, as BigQuery's messages give it: a row takes in the empty lines
  // before it, as BigQuery's answers in external_table_json_autodetect.api.answers.json show.
  std::size_t row_start = 0;
  for (std::size_t start = 0; start < contents.size();) {
    const auto newline = contents.find('\n', start);
    const auto end = newline == std::string::npos ? contents.size() : newline;
    const std::string_view line(contents.data() + start, end - start);
    start = end + 1;
    if (line.find_first_not_of(" \t\r") == std::string_view::npos) {
      // BigQuery reads an empty line, but not one of spaces.
      if (line.find_first_of(" \t") != std::string_view::npos) {
        if (bad != nullptr) {
          ++bad->records;
        }
        Bad(ApiError::Invalid("Unexpected token"), row_start, source, bad);
        row_start = start;
      }
      continue;
    }
    if (bad != nullptr) {
      ++bad->records;
    }
    try {
      const std::optional<std::string> without_comma = WithoutTrailingComma(line);
      const std::string_view row = without_comma ? std::string_view(*without_comma) : line;
      // The row as a read that converts its values or not takes it.
      const auto read = [&](bool converted) {
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
        // NUMERIC and BIGNUMERIC numbers, and with `converted` those of STRING fields, are read
        // as their text, which ValidateValue converts.
        const std::string text =
            QuotesJsonNumerics(schema, converted)
                ? QuotedJsonNumerics(row, schema, converted, /*bignumeric_text=*/true)
                : std::string(row);
        auto object = json::parse(text, unique_keys, false);
        if (object.is_discarded() || !object.is_object()) {
          throw ApiError::Invalid(std::string(NotObjectMessage(line)));
        }
        if (given_schema) {
          LeaveOutUnknownFields(object, schema);
        }
        ValidateObject(object, schema, converted, formats);
        return object;
      };
      if (!given_schema) {
        rewritten += read(converts).dump() + '\n';
      } else {
        // Whether such a read converts values as one of a detected schema does is not known, so
        // a row must read the same either way.
        // A read's row, or its error and the error's message.
        struct Outcome {
          std::optional<json> row = std::nullopt;
          std::exception_ptr error = nullptr;
          std::string message = {};
        };
        const auto outcome = [&read](bool converted) {
          try {
            return Outcome{.row = read(converted)};
          } catch (const ApiError& error) {
            return Outcome{.error = std::current_exception(), .message = error.what()};
          }
        };
        const Outcome with = outcome(true);
        const Outcome without = outcome(false);
        for (const Outcome* read_outcome : {&with, &without}) {
          if (read_outcome->error && read_outcome->message.starts_with(kUnsupported)) {
            std::rethrow_exception(read_outcome->error);
          }
        }
        if (with.error && without.error && with.message == without.message) {
          std::rethrow_exception(with.error);
        }
        if (!with.row || !without.row || *with.row != *without.row) {
          throw ApiError::Unsupported(
              "converting JSON values of an autodetected external table with a given schema");
        }
        rewritten += with.row->dump() + '\n';
      }
    } catch (const ApiError& error) {
      if (std::string_view(error.what()).starts_with(kUnsupported)) {
        throw;
      }
      Bad(error, row_start, source, bad);
    }
    row_start = start;
  }
  return {.uri = source.uri, .path = files.Write(rewritten)};
}

}  // namespace

std::vector<FileSource> StageExternalJson(const std::vector<FileSource>& sources,
                                          const std::vector<FieldSchema>& schema,
                                          TemporaryFiles& files, bool autodetected,
                                          bool given_schema, const DatetimeFormats& formats,
                                          BadRecords* bad) {
  std::vector<FileSource> staged;
  std::ranges::transform(sources, std::back_inserter(staged), [&](const FileSource& source) {
    return StageSource(source, schema, files, autodetected, given_schema, formats, bad);
  });
  return staged;
}

}  // namespace bigquery_emulator_duckdb
