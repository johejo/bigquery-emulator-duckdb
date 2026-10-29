#include "src/translator/literal.h"

#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/strings.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "src/bignumeric.h"
#include "src/duckdb_sql.h"
#include "src/language_options.h"
#include "src/timestamp_nanos.h"
#include "src/timestamp_precision.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {

std::optional<std::string> Literal(const googlesql::Value& value, TimestampPrecision precision) {
  const auto type = DuckDbType(value.type(), nullptr, precision);
  if (!type) {
    return std::nullopt;
  }
  if (value.type()->IsBigNumericType() && !value.is_null()) {
    return BigNumericSql(value.bignumeric_value());
  }
  std::string literal;
  if (value.is_null()) {
    literal = "NULL";
  } else if (value.type()->IsTimestamp() && precision == TimestampPrecision::kNanoseconds) {
    return TimestampNanosSql(value.ToUnixPicos().ToAbslTime());
  } else if (value.type()->IsInterval()) {
    const auto& interval = value.interval_value();
    if (interval.get_nano_fractions() != 0) {
      return std::nullopt;
    }
    return "bq_interval_parts(" + std::to_string(interval.get_months()) + ", " +
           std::to_string(interval.get_days()) + ", " + std::to_string(interval.get_micros()) + ")";
  } else if (value.type()->IsRange()) {
    std::vector<std::string> bounds;
    for (const auto& [bound, unbounded] :
         {std::pair{&value.start(), "-infinity"}, std::pair{&value.end(), "infinity"}}) {
      const auto sql = bound->is_null() ? std::optional<std::string>(QuoteLiteral(unbounded))
                                        : Literal(*bound, precision);
      if (!sql) {
        return std::nullopt;
      }
      if (!bound->is_null() && bound->type()->IsTimestamp() &&
          precision == TimestampPrecision::kNanoseconds) {
        bounds.push_back("bq_timestamp_nanos_to_micros(" + *sql + ")");
      } else {
        bounds.push_back(*sql);
      }
    }
    literal = "struct_pack(" + QuoteIdentifier(kRangeStart) + " := " + bounds.at(0) + ", " +
              QuoteIdentifier(kRangeEnd) + " := " + bounds.at(1) + ")";
  } else if (value.type()->IsString()) {
    literal = QuoteLiteral(value.string_value());
  } else if (value.type()->IsBytes()) {
    literal = "from_hex(" + QuoteLiteral(ToHex(value.bytes_value())) + ")";
  } else if (value.type()->IsArray()) {
    std::vector<std::string> elements;
    for (int i = 0; i < value.num_elements(); ++i) {
      const auto sql = Literal(value.element(i), precision);
      if (!sql) {
        return std::nullopt;
      }
      elements.push_back(*sql);
    }
    literal = "[" + Join(elements, ", ") + "]";
  } else if (value.type()->IsStruct()) {
    const auto names = DuckDbStructFieldNames(value.type()->AsStruct());
    std::vector<std::string> fields;
    for (int i = 0; i < value.num_fields(); ++i) {
      const auto sql = Literal(value.field(i), precision);
      if (!sql) {
        return std::nullopt;
      }
      fields.push_back(QuoteIdentifier(names.at(i)) + " := " + *sql);
    }
    if (fields.empty()) {
      fields.push_back(QuoteIdentifier(kEmptyStruct) + " := TRUE");
    }
    literal = "struct_pack(" + Join(fields, ", ") + ")";
  } else if (value.type()->IsDouble() && value.double_value() == 0 &&
             std::signbit(value.double_value())) {
    // GoogleSQL's SQL literal canonicalizes negative zero; DuckDB preserves its sign when
    // reading a string instead.
    literal = QuoteLiteral("-0");
  } else if (value.type()->IsDouble() && !std::isfinite(value.double_value())) {
    literal = QuoteLiteral(std::isnan(value.double_value()) ? "nan"
                           : value.double_value() < 0       ? "-inf"
                                                            : "inf");
  } else {
    literal = value.GetSQLLiteral(GoogleSqlLanguageOptions());
    if (value.type()->IsDate() || value.type()->IsTimestamp() || value.type()->IsDatetime() ||
        value.type()->IsTime() || value.type()->IsNumericType() || value.type()->IsJson()) {
      std::string contents;
      if (!googlesql::ParseStringLiteral(literal.substr(literal.find(' ') + 1), &contents).ok()) {
        return std::nullopt;
      }
      literal = QuoteLiteral(contents);
    }
  }
  return "CAST(" + literal + " AS " + *type + ")";
}

}  // namespace bigquery_emulator_duckdb::translator
