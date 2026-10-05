// Runs GoogleSQL's compliance tests against the emulator. Each statement goes through
// Emulator::RunQuery as a query job would, with parameters converted to BigQuery's
// QueryParameter form and the result decoded from the rows the job returns.

#include "googlesql/compliance/test_driver.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/globals.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "googlesql/common/internal_value.h"
#include "googlesql/public/civil_time.h"
#include "googlesql/public/functions/date_time_util.h"
#include "googlesql/public/interval_value.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/numeric_value.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/catalog.h"
#include "src/emulator.h"
#include "src/field_schema.h"
#include "src/query_parameters.h"
#include "src/references.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {
namespace {

using googlesql::Value;
using nlohmann::json;

constexpr char kProject[] = "compliance";
constexpr char kDataset[] = "compliance";

// The name a struct field is given in a parameter: BigQuery's `_field_n` when it has none.
std::string FieldName(const googlesql::StructType& type, int index) {
  const std::string& name = type.field(index).name;
  return name.empty() ? absl::StrCat("_field_", index + 1) : name;
}

// A QueryParameter's parameterType. Scalar types take their external name, which
// QueryParameters::Parse rejects for a type BigQuery lacks.
json ParameterType(const googlesql::Type* type) {
  if (type->IsArray()) {
    return {{"type", "ARRAY"}, {"arrayType", ParameterType(type->AsArray()->element_type())}};
  }
  if (type->IsStruct()) {
    const googlesql::StructType& struct_type = *type->AsStruct();
    json fields = json::array();
    for (int i = 0; i < struct_type.num_fields(); ++i) {
      fields.push_back({{"name", FieldName(struct_type, i)},
                        {"type", ParameterType(struct_type.field(i).type)}});
    }
    return {{"type", "STRUCT"}, {"structTypes", std::move(fields)}};
  }
  return {{"type", type->TypeName(googlesql::PRODUCT_EXTERNAL)}};
}

absl::StatusOr<std::string> ScalarText(const Value& value) {
  std::string text;
  switch (value.type_kind()) {
    case googlesql::TYPE_STRING:
      return value.string_value();
    case googlesql::TYPE_BYTES:
      return absl::Base64Escape(value.bytes_value());
    case googlesql::TYPE_BOOL:
      return value.bool_value() ? "true" : "false";
    case googlesql::TYPE_INT64:
      return absl::StrCat(value.int64_value());
    case googlesql::TYPE_DOUBLE: {
      const double number = value.double_value();
      if (std::isnan(number)) return "NaN";
      if (std::isinf(number)) return number > 0 ? "Infinity" : "-Infinity";
      return std::format("{}", number);
    }
    case googlesql::TYPE_NUMERIC:
      return value.numeric_value().ToString();
    case googlesql::TYPE_BIGNUMERIC:
      return value.bignumeric_value().ToString();
    case googlesql::TYPE_DATE:
      GOOGLESQL_RETURN_IF_ERROR(
          googlesql::functions::ConvertDateToString(value.date_value(), &text));
      return text;
    case googlesql::TYPE_TIMESTAMP:
      GOOGLESQL_RETURN_IF_ERROR(googlesql::functions::ConvertTimestampToString(
          value.ToUnixPicos().ToAbslTime(), googlesql::functions::kMicroseconds, "UTC", &text));
      return text;
    case googlesql::TYPE_DATETIME:
      GOOGLESQL_RETURN_IF_ERROR(googlesql::functions::ConvertDatetimeToString(
          value.datetime_value(), googlesql::functions::kMicroseconds, &text));
      return text;
    case googlesql::TYPE_TIME:
      GOOGLESQL_RETURN_IF_ERROR(googlesql::functions::ConvertTimeToString(
          value.time_value(), googlesql::functions::kMicroseconds, &text));
      return text;
    case googlesql::TYPE_INTERVAL:
      return value.interval_value().ToString();
    case googlesql::TYPE_JSON:
      return value.is_unparsed_json() ? value.json_string() : value.json_value().ToString();
    default:
      return absl::UnimplementedError("Unsupported parameter type " + value.type()->DebugString());
  }
}

// A QueryParameter's parameterValue; an empty object is a NULL.
absl::StatusOr<json> ParameterValue(const Value& value) {
  if (value.is_null()) {
    return json::object();
  }
  if (value.type()->IsArray()) {
    json elements = json::array();
    for (int i = 0; i < value.num_elements(); ++i) {
      GOOGLESQL_ASSIGN_OR_RETURN(json element_value, ParameterValue(value.element(i)));
      elements.push_back(std::move(element_value));
    }
    return json{{"arrayValues", std::move(elements)}};
  }
  if (value.type()->IsStruct()) {
    json fields = json::object();
    for (int i = 0; i < value.num_fields(); ++i) {
      GOOGLESQL_ASSIGN_OR_RETURN(fields[FieldName(*value.type()->AsStruct(), i)],
                                 ParameterValue(value.field(i)));
    }
    return json{{"structValues", std::move(fields)}};
  }
  GOOGLESQL_ASSIGN_OR_RETURN(std::string text, ScalarText(value));
  return json{{"value", std::move(text)}};
}

absl::StatusOr<QueryParameters> ToQueryParameters(const std::map<std::string, Value>& parameters) {
  json list = json::array();
  for (const auto& [name, value] : parameters) {
    GOOGLESQL_ASSIGN_OR_RETURN(json parameter_value, ParameterValue(value));
    list.push_back({{"name", name},
                    {"parameterType", ParameterType(value.type())},
                    {"parameterValue", std::move(parameter_value)}});
  }
  try {
    return QueryParameters::Parse(list);
  } catch (const ApiError& error) {
    return absl::UnimplementedError(error.what());
  }
}

absl::Status Undecodable(const googlesql::Type* type, std::string_view text) {
  return absl::InternalError(
      absl::StrCat("Cannot decode ", type->DebugString(), " result \"", text, "\""));
}

// Decodes the value of a result cell, its "v", as the emulator returns it to clients.
absl::StatusOr<Value> CellValue(const googlesql::Type* type, const json& cell) {
  if (cell.is_null()) {
    return Value::Null(type);
  }
  if (type->IsArray()) {
    std::vector<Value> elements;
    for (const json& element : cell) {
      GOOGLESQL_ASSIGN_OR_RETURN(elements.emplace_back(),
                                 CellValue(type->AsArray()->element_type(), element.at("v")));
    }
    return googlesql::InternalValue::ArrayNotChecked(
        type->AsArray(), googlesql::InternalValue::kPreservesOrder, std::move(elements));
  }
  if (type->IsStruct()) {
    std::vector<Value> fields;
    const json& cells = cell.at("f");
    for (int i = 0; i < type->AsStruct()->num_fields(); ++i) {
      GOOGLESQL_ASSIGN_OR_RETURN(fields.emplace_back(),
                                 CellValue(type->AsStruct()->field(i).type, cells.at(i).at("v")));
    }
    return Value::MakeStruct(type->AsStruct(), std::move(fields));
  }
  const std::string text = cell.get<std::string>();
  switch (type->kind()) {
    case googlesql::TYPE_STRING:
      return Value::String(text);
    case googlesql::TYPE_BYTES: {
      std::string bytes;
      if (!absl::Base64Unescape(text, &bytes)) return Undecodable(type, text);
      return Value::Bytes(bytes);
    }
    case googlesql::TYPE_BOOL:
      if (text != "true" && text != "false") return Undecodable(type, text);
      return Value::Bool(text == "true");
    case googlesql::TYPE_INT64: {
      int64_t number = 0;
      if (!absl::SimpleAtoi(text, &number)) return Undecodable(type, text);
      return Value::Int64(number);
    }
    case googlesql::TYPE_DOUBLE: {
      // BigQuery spells NaN and the infinities NaN, Infinity and -Infinity; clients also accept
      // DuckDB's nan and inf.
      double number = 0;
      if (!absl::SimpleAtod(text, &number)) return Undecodable(type, text);
      return Value::Double(number);
    }
    case googlesql::TYPE_NUMERIC: {
      GOOGLESQL_ASSIGN_OR_RETURN(googlesql::NumericValue number,
                                 googlesql::NumericValue::FromString(text));
      return Value::Numeric(number);
    }
    case googlesql::TYPE_BIGNUMERIC: {
      GOOGLESQL_ASSIGN_OR_RETURN(googlesql::BigNumericValue number,
                                 googlesql::BigNumericValue::FromString(text));
      return Value::BigNumeric(number);
    }
    case googlesql::TYPE_DATE: {
      int32_t date = 0;
      GOOGLESQL_RETURN_IF_ERROR(googlesql::functions::ConvertStringToDate(text, &date));
      return Value::Date(date);
    }
    case googlesql::TYPE_TIMESTAMP: {
      // Rows carry epoch microseconds until they are rendered for a client.
      int64_t micros = 0;
      if (!absl::SimpleAtoi(text, &micros) ||
          !googlesql::functions::IsValidTimestamp(micros, googlesql::functions::kMicroseconds)) {
        return Undecodable(type, text);
      }
      return Value::TimestampFromUnixMicros(micros);
    }
    case googlesql::TYPE_DATETIME: {
      googlesql::DatetimeValue datetime;
      GOOGLESQL_RETURN_IF_ERROR(googlesql::functions::ConvertStringToDatetime(
          text, googlesql::functions::kMicroseconds, &datetime));
      return Value::Datetime(datetime);
    }
    case googlesql::TYPE_TIME: {
      googlesql::TimeValue time;
      GOOGLESQL_RETURN_IF_ERROR(googlesql::functions::ConvertStringToTime(
          text, googlesql::functions::kMicroseconds, &time));
      return Value::Time(time);
    }
    case googlesql::TYPE_INTERVAL: {
      GOOGLESQL_ASSIGN_OR_RETURN(googlesql::IntervalValue interval,
                                 googlesql::IntervalValue::ParseFromString(text, false));
      return Value::Interval(interval);
    }
    case googlesql::TYPE_JSON: {
      GOOGLESQL_ASSIGN_OR_RETURN(googlesql::JSONValue value,
                                 googlesql::JSONValue::ParseJSONString(text));
      return Value::Json(std::move(value));
    }
    default:
      return Undecodable(type, text);
  }
}

// The rows of a query result as the ARRAY<STRUCT> the compliance tests compare.
//
// TODO(johejo): Results carry a NULL array as an empty one, as BigQuery's do, so a case that
// expects a NULL array fails although no client can tell the two apart; about 500 cases fail only
// for that. Compare such arrays as equal instead.
absl::StatusOr<Value> ResultValue(const QueryResult& result, googlesql::TypeFactory* type_factory) {
  std::vector<googlesql::StructType::StructField> columns;
  for (const FieldSchema& field : result.schema) {
    GOOGLESQL_ASSIGN_OR_RETURN(const googlesql::Type* type, GoogleSqlType(field, type_factory));
    columns.emplace_back(field.name, type);
  }
  const googlesql::StructType* row_type = nullptr;
  GOOGLESQL_RETURN_IF_ERROR(type_factory->MakeStructType(columns, &row_type));
  GOOGLESQL_ASSIGN_OR_RETURN(const googlesql::ArrayType* table_type,
                             type_factory->MakeArrayType(row_type));
  std::vector<Value> rows;
  for (const json& row : result.rows) {
    GOOGLESQL_ASSIGN_OR_RETURN(rows.emplace_back(), CellValue(row_type, row));
  }
  // ORDER BY is not checked: the result does not say whether the query ordered its rows.
  return googlesql::InternalValue::ArrayNotChecked(
      table_type, googlesql::InternalValue::kIgnoresOrder, std::move(rows));
}

// The status a failed job stands for. Analysis errors keep GoogleSQL's status text; DuckDB
// errors that mean the translation is wrong are internal, which no expected error matches;
// any other failure is a runtime error.
absl::Status JobStatus(const ApiError& error) {
  const std::string_view message = error.what();
  if (message.starts_with("The emulator does not support")) {
    return absl::UnimplementedError(message);
  }
  if (message.starts_with("INVALID_ARGUMENT:")) {
    return absl::InvalidArgumentError(message);
  }
  for (const std::string_view prefix :
       {"Binder Error", "Parser Error", "Catalog Error", "Not implemented Error", "INTERNAL"}) {
    if (message.starts_with(prefix)) {
      return absl::InternalError(message);
    }
  }
  return absl::OutOfRangeError(message);
}

class EmulatorTestDriver : public googlesql::TestDriver {
 public:
  EmulatorTestDriver() { Reset(); }

  // The emulator's options, without the types and precision BigQuery lacks, so that the reference
  // implementation evaluates what BigQuery would and tests that need them are skipped.
  // TODO(johejo): Drop these features from GoogleSqlLanguageOptions() itself, then use it as is.
  googlesql::LanguageOptions GetSupportedLanguageOptions() override {
    googlesql::LanguageOptions options = GoogleSqlLanguageOptions();
    for (const googlesql::LanguageFeature feature :
         {googlesql::FEATURE_TIMESTAMP_NANOS, googlesql::FEATURE_MAP_TYPE,
          googlesql::FEATURE_UUID_TYPE}) {
      options.DisableLanguageFeature(feature);
    }
    options.EnableLanguageFeature(googlesql::FEATURE_DISABLE_FLOAT32);
    return options;
  }

  // Queries run in UTC, BigQuery's default time zone, unless they name another.
  const absl::TimeZone GetDefaultTimeZone() const override { return absl::UTCTimeZone(); }

  absl::Status SetDefaultTimeZone(const std::string& default_time_zone) override {
    if (default_time_zone != "UTC") {
      return absl::UnimplementedError("Default time zone " + default_time_zone);
    }
    return absl::OkStatus();
  }

  absl::Status CreateDatabase(const googlesql::TestDatabase& test_db) override {
    Reset();
    for (const auto& [name, table] : test_db.tables) {
      // Value tables, which BigQuery lacks, are left out, and so are the columns of types it
      // lacks; the tests that read them fail to resolve, or are skipped for using a type the
      // engine does not support.
      if (table.options.is_value_table()) continue;
      const googlesql::StructType& row_type =
          *table.table_as_value.type()->AsArray()->element_type()->AsStruct();
      std::vector<FieldSchema> schema;
      std::vector<int> columns;
      for (int i = 0; i < row_type.num_fields(); ++i) {
        const googlesql::StructType::StructField& column = row_type.field(i);
        absl::StatusOr<FieldSchema> field = BigQueryFieldSchema(column.name, column.type);
        if (field.ok() && column.type->IsSupportedType(GetSupportedLanguageOptions())) {
          schema.push_back(*std::move(field));
          columns.push_back(i);
        }
      }
      if (schema.empty()) continue;
      if (absl::Status loaded = LoadTable(name, schema, table.table_as_value, columns);
          !loaded.ok()) {
        LOG(WARNING) << "Cannot load table " << name << ": " << loaded;
      }
    }
    return absl::OkStatus();
  }

  absl::StatusOr<Value> ExecuteStatement(const std::string& sql,
                                         const std::map<std::string, Value>& parameters,
                                         googlesql::TypeFactory* type_factory) override {
    GOOGLESQL_ASSIGN_OR_RETURN(QueryParameters query_parameters, ToQueryParameters(parameters));
    const std::shared_ptr<const Job> job =
        emulator_->RunQuery(QueryRequest{.project_id = kProject,
                                         .query = sql,
                                         .default_dataset = DatasetReference{kProject, kDataset},
                                         .parameters = std::move(query_parameters)});
    if (job->error.has_value()) {
      return JobStatus(*job->error);
    }
    if (job->query()->statement_type != "SELECT" || !job->result.has_value()) {
      return absl::UnimplementedError("Only queries are compared: " + sql);
    }
    return ResultValue(*job->result, type_factory);
  }

 private:
  // Creates the table `name` with `schema` and inserts the given `columns` of `rows`.
  absl::Status LoadTable(const std::string& name, const std::vector<FieldSchema>& schema,
                         const Value& rows, const std::vector<int>& columns) {
    try {
      emulator_->CreateTable(TableReference{kProject, kDataset, name}, schema);
    } catch (const ApiError& error) {
      return absl::UnimplementedError(error.what());
    }
    if (rows.empty()) return absl::OkStatus();
    const googlesql::StructType& row_type = *rows.type()->AsArray()->element_type()->AsStruct();
    std::vector<googlesql::StructType::StructField> fields;
    fields.reserve(columns.size());
    std::ranges::transform(columns, std::back_inserter(fields),
                           [&](int column) { return row_type.field(column); });
    const googlesql::StructType* kept_type = nullptr;
    GOOGLESQL_RETURN_IF_ERROR(type_factory_.MakeStructType(fields, &kept_type));
    GOOGLESQL_ASSIGN_OR_RETURN(const googlesql::ArrayType* table_type,
                               type_factory_.MakeArrayType(kept_type));
    std::vector<Value> kept_rows;
    for (int i = 0; i < rows.num_elements(); ++i) {
      std::vector<Value> values;
      values.reserve(columns.size());
      std::ranges::transform(columns, std::back_inserter(values),
                             [&](int column) { return rows.element(i).field(column); });
      GOOGLESQL_ASSIGN_OR_RETURN(kept_rows.emplace_back(),
                                 Value::MakeStruct(kept_type, std::move(values)));
    }
    GOOGLESQL_ASSIGN_OR_RETURN(Value kept, Value::MakeArray(table_type, std::move(kept_rows)));
    GOOGLESQL_ASSIGN_OR_RETURN(QueryParameters parameters, ToQueryParameters({{"rows", kept}}));
    const std::shared_ptr<const Job> job = emulator_->RunQuery(
        QueryRequest{.project_id = kProject,
                     .query = absl::StrCat("INSERT INTO `", name, "` SELECT * FROM UNNEST(@rows)"),
                     .default_dataset = DatasetReference{kProject, kDataset},
                     .parameters = std::move(parameters)});
    if (job->error.has_value()) {
      return JobStatus(*job->error);
    }
    return absl::OkStatus();
  }

  void Reset() {
    emulator_ = std::make_unique<Emulator>();
    emulator_->CreateDataset(DatasetReference{kProject, kDataset});
  }

  googlesql::TypeFactory type_factory_;
  std::unique_ptr<Emulator> emulator_;
};

}  // namespace
}  // namespace bigquery_emulator_duckdb

namespace googlesql {

TestDriver* GetComplianceTestDriver() {
  // The compliance report and the known errors it suggests are logged at INFO, and this binary
  // does not link Abseil's logging flags.
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);
  return new bigquery_emulator_duckdb::EmulatorTestDriver();
}

}  // namespace googlesql
