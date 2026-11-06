#include "src/translator/ddl_options.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/time/time.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/api_error.h"
#include "src/bigtable_options.h"
#include "src/datetime_formats.h"
#include "src/ddl_option_types.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/language_options.h"
#include "src/location.h"
#include "src/table_metadata.h"
#include "src/text_encoding.h"
#include "src/translated_statement.h"
#include "src/translator/context.h"
#include "src/translator/expression.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

// TIMESTAMP options use the same millisecond precision as Table.expirationTime.
bool ExpirationOption(const googlesql::Value& value, std::optional<int64_t>& into) {
  if (value.is_null()) {
    into.reset();
    return true;
  }
  if (!value.type()->IsTimestamp()) {
    return false;
  }
  into = absl::ToUnixMillis(value.ToUnixPicos().ToAbslTime());
  return true;
}

// The default_table_expiration_days option, a FLOAT64 number of days, in the milliseconds of
// Dataset.defaultTableExpirationMs; 0, as there, and NULL clear it.
bool DaysOption(const googlesql::Value& value, std::optional<int64_t>& into) {
  if (value.is_null()) {
    into.reset();
    return true;
  }
  if (!value.type()->IsDouble() && !value.type()->IsInt64()) {
    return false;
  }
  constexpr double kMillisecondsPerDay = 86400000;
  const double milliseconds = std::round(
      (value.type()->IsDouble() ? value.double_value() : static_cast<double>(value.int64_value())) *
      kMillisecondsPerDay);
  // Days whose milliseconds do not fit in an INT64 are rejected.
  if (!std::isfinite(milliseconds) || std::abs(milliseconds) >= 0x1p63) {
    return false;
  }
  into = milliseconds == 0 ? std::nullopt : std::optional(static_cast<int64_t>(milliseconds));
  return true;
}

bool TimeTravelOption(const googlesql::Value& value, std::optional<int64_t>& into) {
  if (!value.is_null() && !value.type()->IsInt64()) {
    return false;
  }
  into = value.is_null() ? std::nullopt : std::optional(value.int64_value());
  ValidateMaxTimeTravelHours(into);
  return true;
}

bool StorageBillingModelOption(const googlesql::Value& value, std::string& into) {
  if (value.is_null()) {
    return true;
  }
  if (!value.type()->IsString()) {
    return false;
  }
  const std::string model = ToUpperAscii(value.string_value());
  if (model != "LOGICAL" && model != "PHYSICAL") {
    throw ApiError::InvalidQuery(std::format(
        "'{}' is not a valid value; failed to set 'storage_billing_model' in schema OPTIONS",
        value.string_value()));
  }
  into = model;
  return true;
}

bool BoolOption(const googlesql::Value& value, bool& into) {
  if (!value.is_null() && !value.type()->IsBool()) {
    return false;
  }
  into = !value.is_null() && value.bool_value();
  return true;
}

// The labels option, an ARRAY<STRUCT<STRING, STRING>> of keys and values, into `into`.
bool LabelsOption(const googlesql::Value& value, std::map<std::string, std::string>& into,
                  std::vector<std::string>& order) {
  into.clear();
  order.clear();
  if (value.is_null()) {
    return true;
  }
  const googlesql::Type* type = value.type();
  if (!type->IsArray() || !type->AsArray()->element_type()->IsStruct()) {
    return false;
  }
  const googlesql::StructType* entry = type->AsArray()->element_type()->AsStruct();
  if (entry->num_fields() != 2 || !entry->field(0).type->IsString() ||
      !entry->field(1).type->IsString()) {
    return false;
  }
  for (int i = 0; i < value.num_elements(); ++i) {
    const googlesql::Value& element = value.element(i);
    if (element.is_null() || element.field(0).is_null() || element.field(1).is_null()) {
      return false;
    }
    const std::string& key = element.field(0).string_value();
    if (!into.contains(key)) {
      order.push_back(key);
    }
    into[key] = element.field(1).string_value();
  }
  return true;
}

// Tags have the same pair representation as labels, but cannot repeat a key.
// NULL clears tags on replacement; incremental NULL operands are handled by SetOptions.
bool TagsOption(const googlesql::Value& value, std::map<std::string, std::string>& into,
                std::vector<std::string>& order) {
  if (!LabelsOption(value, into, order) ||
      (!value.is_null() && static_cast<size_t>(value.num_elements()) != into.size())) {
    return false;
  }
  ValidateResourceTags(into);
  return true;
}

// The scalar functions that BigQuery's DDL reference excludes from option values.
constexpr auto kExcludedOptionFunctions = std::to_array<std::string_view>({
    "ARRAY_TO_STRING",
    "REPLACE",
    "REGEXP_REPLACE",
    "RAND",
    "FORMAT",
    "LPAD",
    "RPAD",
    "REPEAT",
    "SESSION_USER",
    "GENERATE_ARRAY",
    "GENERATE_DATE_ARRAY",
});

// Whether `expression`, the value of `option` of `object` such as a table, is what BigQuery's DDL
// reference allows as an option value: "a constant expression containing only literals, query
// parameters, and scalar functions", other than the functions it excludes, which fail. Anything
// else, such as a script variable in parentheses, is unsupported.
bool ConstantOptionValue(const googlesql::ResolvedExpr& expression, std::string_view option,
                         std::string_view object, const Scope& scope) {
  const auto constant = [&](const googlesql::ResolvedExpr& element) {
    return ConstantOptionValue(element, option, object, scope);
  };
  const auto all = [&](const auto& expressions) {
    return std::ranges::all_of(expressions,
                               [&](const auto& element) { return constant(*element); });
  };
  if (expression.Is<googlesql::ResolvedLiteral>() ||
      expression.Is<googlesql::ResolvedParameter>()) {
    return true;
  }
  if (expression.Is<googlesql::ResolvedFunctionCall>()) {
    const auto* call = expression.GetAs<googlesql::ResolvedFunctionCall>();
    const std::string name = ToUpperAscii(call->function()->Name());
    if (std::ranges::find(kExcludedOptionFunctions, name) != kExcludedOptionFunctions.end()) {
      throw std::invalid_argument("Found unsupported function call '" + name +
                                  "'; failed to evaluate '" + std::string(option) + "' in " +
                                  std::string(object) + " OPTIONS");
    }
    if (!IsBuiltinFunction(*call->function())) {
      Unsupported(scope, "function " + name + " in option values");
      return false;
    }
    return all(call->argument_list());
  }
  if (expression.Is<googlesql::ResolvedCast>()) {
    const auto* cast = expression.GetAs<googlesql::ResolvedCast>();
    return constant(*cast->expr()) && (cast->format() == nullptr || constant(*cast->format())) &&
           (cast->time_zone() == nullptr || constant(*cast->time_zone()));
  }
  if (expression.Is<googlesql::ResolvedMakeStruct>()) {
    const auto* make = expression.GetAs<googlesql::ResolvedMakeStruct>();
    return all(make->field_list());
  }
  Unsupported(scope, "option values other than literals, query parameters and scalar functions");
  return false;
}

// The object whose options `statement`, such as CREATE TABLE, sets, as BigQuery's errors name it.
std::string OptionObject(std::string_view statement) {
  if (statement.ends_with("FUNCTION")) {
    return "function";
  }
  return ToLowerAscii(statement.substr(statement.find(' ') + 1));
}

// A location as CanonicalLocation spells it; NULL leaves it unset.
bool LocationOption(const googlesql::Value& value, std::string& into) {
  std::string location;
  if (!StringOption(value, location)) {
    return false;
  }
  if (location.empty()) {
    into.clear();
    return true;
  }
  std::optional<std::string> canonical = CanonicalLocation(location);
  if (!canonical.has_value()) {
    throw ApiError::InvalidQuery("Invalid location '" + location + "'");
  }
  into = *std::move(canonical);
  return true;
}

bool ApplyTableOption(DdlOptionId name, const googlesql::Value& value, bool table,
                      TableMetadata& metadata) {
  using enum DdlOptionId;
  switch (name) {
    case kRequirePartitionFilter:
      return table && BoolOption(value, metadata.require_partition_filter);
    case kPartitionExpirationDays:
    case kKmsKeyName:
      return table && value.is_null();
    case kDescription:
      return StringOption(value, metadata.description);
    case kFriendlyName:
      return StringOption(value, metadata.friendly_name);
    case kLabels:
      return LabelsOption(value, metadata.labels, metadata.label_order);
    case kTags:
      return TagsOption(value, metadata.resource_tags, metadata.tag_order);
    case kExpirationTimestamp:
      return ExpirationOption(value, metadata.expiration_time);
    case kDefaultRoundingMode:
      return table && RoundingModeOption(value, metadata.default_rounding_mode);
    default:
      return false;
  }
}

bool ApplySchemaOption(DdlOptionId name, const googlesql::Value& value, DatasetMetadata& metadata) {
  using enum DdlOptionId;
  switch (name) {
    case kDescription:
      return StringOption(value, metadata.description);
    case kFriendlyName:
      return StringOption(value, metadata.friendly_name);
    case kLabels:
      return LabelsOption(value, metadata.labels, metadata.label_order);
    case kTags:
      return TagsOption(value, metadata.resource_tags, metadata.tag_order);
    case kDefaultTableExpirationDays:
      return DaysOption(value, metadata.default_table_expiration_ms);
    case kMaxTimeTravelHours:
      return TimeTravelOption(value, metadata.max_time_travel_hours);
    case kIsCaseInsensitive:
      return !value.is_null() && value.bool_value() &&
             BoolOption(value, metadata.is_case_insensitive.emplace());
    case kStorageBillingModel:
      return StorageBillingModelOption(value, metadata.storage_billing_model);
    case kDefaultRoundingMode:
      return RoundingModeOption(value, metadata.default_rounding_mode);
    case kLocation:
      return LocationOption(value, metadata.location);
    default:
      return false;
  }
}

bool ApplyOptionUpdate(DdlOptionId name, const googlesql::Value& value, DdlOptionContext statement,
                       OptionUpdates& updates) {
  using enum DdlOptionId;
  const bool table = statement == DdlOptionContext::kAlterTable;
  const bool schema = statement == DdlOptionContext::kAlterSchema;
  switch (name) {
    case kRequirePartitionFilter:
      return table && BoolOption(value, updates.require_partition_filter.emplace());
    case kDescription:
      return StringOption(value, updates.description.emplace());
    case kFriendlyName:
      return StringOption(value, updates.friendly_name.emplace());
    case kLabels:
      return LabelsOption(value, updates.labels.emplace(), updates.label_order);
    case kTags:
      return TagsOption(value, updates.resource_tags.emplace(), updates.tag_order);
    case kExpirationTimestamp:
      return table && ExpirationOption(value, updates.expiration_time.emplace());
    case kDefaultTableExpirationDays:
      return schema && DaysOption(value, updates.default_table_expiration_ms.emplace());
    case kMaxTimeTravelHours:
      return schema && TimeTravelOption(value, updates.max_time_travel_hours.emplace());
    case kIsCaseInsensitive:
      return schema && !value.is_null() && value.bool_value() &&
             BoolOption(value, updates.is_case_insensitive.emplace());
    case kStorageBillingModel:
      return schema && (value.is_null() ||
                        StorageBillingModelOption(value, updates.storage_billing_model.emplace()));
    case kDefaultRoundingMode:
      return (table || schema) &&
             RoundingModeOption(value, updates.default_rounding_mode.emplace());
    default:
      return false;
  }
}

// Decodes an option after name-only checks, without resolving its name again.
std::optional<DecodedDdlOption> DecodeNamedDdlOption(const googlesql::ResolvedOption& option,
                                                     std::optional<DdlOptionId> name,
                                                     DdlOptionContext context, const Scope& scope) {
  const auto context_name = DdlOptionContextName(context);
  const std::string object =
      context == DdlOptionContext::kColumn ? "column" : OptionObject(context_name);
  const auto* definition = name ? FindDdlOption(context, *name) : nullptr;
  if (definition == nullptr) {
    Unsupported(scope, std::string(context_name) + " option " + option.name());
    return std::nullopt;
  }
  const auto checked =
      [&](std::optional<googlesql::Value> value) -> std::optional<DecodedDdlOption> {
    const bool bigtable_uri =
        value && definition->name == DdlOptionId::kUris && value->type()->IsString();
    if (!value || (!bigtable_uri && !DdlOptionValueMatches(definition->type, *value))) {
      return std::nullopt;
    }
    return DecodedDdlOption{.id = definition->name, .value = *std::move(value)};
  };
  const bool incremental_tags =
      (context == DdlOptionContext::kAlterTable || context == DdlOptionContext::kAlterSchema) &&
      definition->name == DdlOptionId::kTags &&
      (option.assignment_op() == googlesql::ResolvedOption::ADD_ASSIGN ||
       option.assignment_op() == googlesql::ResolvedOption::SUB_ASSIGN);
  if (!option.qualifier().empty() ||
      (option.assignment_op() != googlesql::ResolvedOption::DEFAULT_ASSIGN && !incremental_tags)) {
    return std::nullopt;
  }
  const googlesql::ResolvedExpr& expression = *option.value();
  if (expression.Is<googlesql::ResolvedLiteral>()) {
    return checked(expression.GetAs<googlesql::ResolvedLiteral>()->value());
  }
  const auto& evaluate = scope.context.defaults.evaluate;
  if (!evaluate || !ConstantOptionValue(expression, option.name(), object, scope)) {
    return std::nullopt;
  }
  const std::optional<std::string> sql = Expression(expression, scope, {});
  if (!sql.has_value()) {
    return std::nullopt;
  }
  return checked(evaluate("SELECT " + *sql, expression.type()));
}

}  // namespace

std::optional<DecodedDdlOption> DecodeDdlOption(const googlesql::ResolvedOption& option,
                                                DdlOptionContext context, const Scope& scope) {
  return DecodeNamedDdlOption(option, ParseDdlOptionId(ToLowerAscii(option.name())), context,
                              scope);
}

// A STRING option into `into`; NULL leaves it unset.
bool DefaultCollation(const googlesql::ResolvedExpr* expression, const Scope& scope,
                      std::optional<std::string>& into) {
  if (expression == nullptr) {
    return true;
  }
  if (!expression->Is<googlesql::ResolvedLiteral>() ||
      expression->GetAs<googlesql::ResolvedLiteral>()->value().is_null()) {
    Unsupported(scope, "default collation that is not a non-NULL literal");
    return false;
  }
  into = expression->GetAs<googlesql::ResolvedLiteral>()->value().string_value();
  ValidateCollation(*into);
  return true;
}

bool StringOption(const googlesql::Value& value, std::string& into) {
  if (value.is_null()) {
    into.clear();
    return true;
  }
  if (!value.type()->IsString()) {
    return false;
  }
  into = value.string_value();
  return true;
}

bool RoundingModeOption(const googlesql::Value& value, RoundingMode& into) {
  std::string mode;
  if (!StringOption(value, mode) || mode == "ROUNDING_MODE_UNSPECIFIED") {
    return false;
  }
  const auto parsed = ParseRoundingMode(mode);
  if (!parsed) {
    return false;
  }
  into = *parsed;
  return true;
}

// The metadata the OPTIONS of a CREATE TABLE or CREATE VIEW set; only a table has a default
// rounding mode.
std::optional<TableMetadata> OptionsMetadata(const Options& options, DdlOptionContext statement,
                                             const Scope& scope, TableMetadata metadata) {
  const bool table = statement == DdlOptionContext::kCreateTable;
  const bool external = statement == DdlOptionContext::kCreateExternalTable;
  bool format = false;
  bool scalar_uri = false;
  if (external) {
    metadata.external_data.emplace();
  }
  for (const auto& option : options) {
    const auto decoded = DecodeDdlOption(*option, statement, scope);
    if (decoded && external && decoded->id == DdlOptionId::kFormat) {
      format = !decoded->value.is_null() &&
               (decoded->value.string_value() == "PARQUET" ||
                decoded->value.string_value() == "CSV" || decoded->value.string_value() == "JSON" ||
                decoded->value.string_value() == "NEWLINE_DELIMITED_JSON" ||
                decoded->value.string_value() == "CLOUD_BIGTABLE");
      if (!format) {
        return Unsupported(scope,
                           "external tables other than PARQUET, CSV, JSON or CLOUD_BIGTABLE");
      }
      metadata.external_data->format =
          decoded->value.string_value() == "JSON"             ? "NEWLINE_DELIMITED_JSON"
          : decoded->value.string_value() == "CLOUD_BIGTABLE" ? "BIGTABLE"
                                                              : decoded->value.string_value();
      continue;
    }
    if (decoded && external && decoded->id == DdlOptionId::kBigtableOptions) {
      if (!decoded->value.is_null()) {
        metadata.external_data->bigtable_options =
            ParseBigtableOptions(decoded->value.string_value());
      }
      continue;
    }
    if (decoded && external && decoded->id == DdlOptionId::kSkipLeadingRows) {
      if (!decoded->value.is_null()) {
        metadata.external_data->skip_leading_rows = decoded->value.int64_value();
      }
      continue;
    }
    if (decoded && external && decoded->id == DdlOptionId::kFieldDelimiter) {
      if (!decoded->value.is_null()) {
        metadata.external_data->field_delimiter = decoded->value.string_value();
      }
      continue;
    }
    if (decoded && external && decoded->id == DdlOptionId::kQuote) {
      if (!decoded->value.is_null()) {
        metadata.external_data->quote = decoded->value.string_value();
      }
      continue;
    }
    // BigQuery's answers in external_table_read_options.api.answers.json: the DDL takes GZIP and
    // the encodings in any case, and stores their REST names.
    if (decoded && external && decoded->id == DdlOptionId::kCompression) {
      if (!decoded->value.is_null()) {
        const std::string& compression = decoded->value.string_value();
        if (!absl::EqualsIgnoreCase(compression, "GZIP")) {
          throw ApiError::InvalidQuery(
              std::format("'{}' is not a valid value; failed to set 'compression' in external "
                          "table OPTIONS",
                          compression));
        }
        metadata.external_data->compression = "GZIP";
      }
      continue;
    }
    if (decoded && external && decoded->id == DdlOptionId::kEncoding) {
      if (!decoded->value.is_null()) {
        const std::string& name = decoded->value.string_value();
        const std::optional<TextEncoding> encoding = ParseTextEncoding(name, /*ddl=*/true);
        if (!encoding) {
          throw ApiError::InvalidQuery(std::format(
              "'{}' is not a valid value; failed to set 'encoding' in external table OPTIONS",
              name));
        }
        metadata.external_data->encoding = std::string(TextEncodingName(*encoding));
      }
      continue;
    }
    if (decoded && external &&
        (decoded->id == DdlOptionId::kTimeZone || decoded->id == DdlOptionId::kDateFormat ||
         decoded->id == DdlOptionId::kDatetimeFormat || decoded->id == DdlOptionId::kTimeFormat ||
         decoded->id == DdlOptionId::kTimestampFormat)) {
      if (!decoded->value.is_null()) {
        GcsExternalData& data = *metadata.external_data;
        std::optional<std::string>& value =
            decoded->id == DdlOptionId::kTimeZone         ? data.time_zone
            : decoded->id == DdlOptionId::kDateFormat     ? data.date_format
            : decoded->id == DdlOptionId::kDatetimeFormat ? data.datetime_format
            : decoded->id == DdlOptionId::kTimeFormat     ? data.time_format
                                                          : data.timestamp_format;
        value = decoded->value.string_value();
      }
      continue;
    }
    if (decoded && external && decoded->id == DdlOptionId::kMaxBadRecords) {
      if (!decoded->value.is_null()) {
        if (decoded->value.int64_value() < 0) {
          throw ApiError::InvalidQuery("max_bad_records may not be negative.");
        }
        metadata.external_data->max_bad_records = decoded->value.int64_value();
      }
      continue;
    }
    if (decoded && external && decoded->id == DdlOptionId::kNullMarker) {
      if (!decoded->value.is_null()) {
        metadata.external_data->null_marker = decoded->value.string_value();
      }
      continue;
    }
    if (decoded && external && decoded->id == DdlOptionId::kNullMarkers) {
      if (!decoded->value.is_null()) {
        auto& markers = metadata.external_data->null_markers.emplace();
        for (int i = 0; i < decoded->value.num_elements(); ++i) {
          if (decoded->value.element(i).is_null()) {
            return Unsupported(scope, "NULL elements of null_markers");
          }
          markers.push_back(decoded->value.element(i).string_value());
        }
      }
      continue;
    }
    std::optional<bool> GcsExternalData::* flag = nullptr;
    if (decoded && external) {
      switch (decoded->id) {
        case DdlOptionId::kAllowQuotedNewlines:
          flag = &GcsExternalData::allow_quoted_newlines;
          break;
        case DdlOptionId::kAllowJaggedRows:
          flag = &GcsExternalData::allow_jagged_rows;
          break;
        case DdlOptionId::kPreserveAsciiControlCharacters:
          flag = &GcsExternalData::preserve_ascii_control_characters;
          break;
        case DdlOptionId::kIgnoreUnknownValues:
          flag = &GcsExternalData::ignore_unknown_values;
          break;
        default:
          break;
      }
    }
    if (flag != nullptr) {
      if (!decoded->value.is_null()) {
        (*metadata.external_data).*flag = decoded->value.bool_value();
      }
      continue;
    }
    if (decoded && external && decoded->id == DdlOptionId::kHivePartitionUriPrefix) {
      if (!decoded->value.is_null()) {
        auto& hive = metadata.external_data->hive_partitioning;
        (hive ? *hive : hive.emplace()).source_uri_prefix = decoded->value.string_value();
      }
      continue;
    }
    if (decoded && external && decoded->id == DdlOptionId::kRequireHivePartitionFilter) {
      if (!decoded->value.is_null()) {
        auto& hive = metadata.external_data->hive_partitioning;
        (hive ? *hive : hive.emplace()).require_partition_filter = decoded->value.bool_value();
      }
      continue;
    }
    if (decoded && external && decoded->id == DdlOptionId::kEnableListInference) {
      if (!decoded->value.is_null()) {
        metadata.external_data->enable_list_inference = decoded->value.bool_value();
      }
      continue;
    }
    if (decoded && external && decoded->id == DdlOptionId::kUris) {
      if (decoded->value.is_null()) {
        return Unsupported(scope, "NULL external table uris");
      }
      if (decoded->value.type()->IsString()) {
        scalar_uri = true;
        metadata.external_data->uris.push_back(decoded->value.string_value());
        continue;
      }
      for (int i = 0; i < decoded->value.num_elements(); ++i) {
        const auto& uri = decoded->value.element(i);
        if (uri.is_null()) {
          return Unsupported(scope, "NULL external table URI");
        }
        metadata.external_data->uris.push_back(uri.string_value());
      }
      continue;
    }
    if (!decoded || !ApplyTableOption(decoded->id, decoded->value, table, metadata)) {
      return Unsupported(
          scope, std::string(DdlOptionContextName(statement)) + " option " + option->name());
    }
  }
  if (external && (!format || metadata.external_data->uris.empty())) {
    return Unsupported(scope, "CREATE EXTERNAL TABLE without format and uris");
  }
  if (external && scalar_uri != (metadata.external_data->format == "BIGTABLE")) {
    throw ApiError::InvalidQuery(
        "Bigtable uris must be a STRING; other external table uris must be an ARRAY<STRING>");
  }
  if (external && metadata.external_data->bigtable_options &&
      metadata.external_data->format != "BIGTABLE") {
    throw ApiError::UnsupportedQuery("bigtable_options on a non-Bigtable external table");
  }
  // As BigQuery's answers in external_table_datetime_formats.api.answers.json show.
  if (external) {
    const GcsExternalData& data = *metadata.external_data;
    for (const auto& [name, value] : {
             std::pair{"time_zone", &data.time_zone},
             {"date_format", &data.date_format},
             {"datetime_format", &data.datetime_format},
             {"time_format", &data.time_format},
             {"timestamp_format", &data.timestamp_format},
         }) {
      if (*value && data.format == "PARQUET") {
        throw ApiError::InvalidQuery(
            std::format("Option '{}' not supported for format PARQUET", name));
      }
    }
    if (const absl::Status formats = CheckDatetimeFormats(ExternalFormats(data), /*ddl=*/true);
        !formats.ok()) {
      throw ApiError::InvalidQuery(std::string(formats.message()));
    }
  }
  return metadata;
}

// The description, friendly name, labels, default table expiration, default rounding mode and
// location the OPTIONS of a CREATE SCHEMA set. The other options are unsupported, since they change
// how BigQuery treats the dataset's tables. Throws ApiError for a location BigQuery does not have.
std::optional<DatasetMetadata> SchemaOptionsMetadata(const Options& options, const Scope& scope) {
  DatasetMetadata metadata;
  for (const auto& option : options) {
    const auto decoded = DecodeDdlOption(*option, DdlOptionContext::kCreateSchema, scope);
    if (!decoded || !ApplySchemaOption(decoded->id, decoded->value, metadata)) {
      return Unsupported(scope, "CREATE SCHEMA option " + option->name());
    }
  }
  return metadata;
}

// The metadata SET OPTIONS of `statement` sets on `updates`. The
// other options change how BigQuery stores or reads the table or dataset, which the
// emulator does not emulate, so they are unsupported. Tags also accept += and -=.
bool SetOptions(const Options& options, DdlOptionContext statement, const Scope& scope,
                OptionUpdates& updates) {
  using enum DdlOptionId;
  bool tags_seen = false;
  const bool schema = statement == DdlOptionContext::kAlterSchema;
  for (const auto& option : options) {
    const auto name = ParseDdlOptionId(ToLowerAscii(option->name()));
    if (schema && name == kLocation) {
      throw ApiError::InvalidQuery("Unknown option: " + option->name());
    }
    if (name == kTags) {
      if (tags_seen) {
        Unsupported(scope, "repeated tags in SET OPTIONS");
        return false;
      }
      tags_seen = true;
    }
    const auto decoded = DecodeNamedDdlOption(*option, name, statement, scope);
    const auto fail = [&] {
      Unsupported(scope,
                  std::string(DdlOptionContextName(statement)) + " option " + option->name());
      return false;
    };
    if (name == kTags && option->assignment_op() != googlesql::ResolvedOption::DEFAULT_ASSIGN) {
      if (!decoded) {
        return fail();
      }
      if (decoded->value.is_null()) {
        continue;
      }
      if (option->assignment_op() == googlesql::ResolvedOption::SUB_ASSIGN) {
        std::vector<std::string> order;
        if (!TagsOption(decoded->value, updates.removed_tags, order)) {
          return fail();
        }
      } else {
        updates.merge_resource_tags = true;
        if (!TagsOption(decoded->value, updates.resource_tags.emplace(), updates.tag_order)) {
          return fail();
        }
      }
      continue;
    }
    if (!decoded || !ApplyOptionUpdate(decoded->id, decoded->value, statement, updates)) {
      return fail();
    }
  }
  return true;
}

}  // namespace bigquery_emulator_duckdb::translator
