#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "googlesql/public/strings.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "src/catalog.h"
#include "src/duckdb_sql.h"
#include "src/translator/internal.h"

namespace bigquery_emulator_duckdb::translator {

std::optional<std::string> Literal(const googlesql::Value& value) {
  const auto type = DuckDbType(value.type());
  if (!type) {
    return std::nullopt;
  }
  std::string literal;
  if (value.is_null()) {
    literal = "NULL";
  } else if (value.type()->IsString()) {
    literal = QuoteLiteral(value.string_value());
  } else if (value.type()->IsBytes()) {
    literal = "from_hex(" + QuoteLiteral(ToHex(value.bytes_value())) + ")";
  } else if (value.type()->IsArray()) {
    std::vector<std::string> elements;
    for (int i = 0; i < value.num_elements(); ++i) {
      const auto sql = Literal(value.element(i));
      if (!sql) {
        return std::nullopt;
      }
      elements.push_back(*sql);
    }
    literal = "[" + Join(elements, ", ") + "]";
  } else if (value.type()->IsStruct()) {
    std::vector<std::string> fields;
    for (int i = 0; i < value.num_fields(); ++i) {
      const auto sql = Literal(value.field(i));
      if (!sql) {
        return std::nullopt;
      }
      fields.push_back(QuoteIdentifier(value.type()->AsStruct()->field(i).name) + " := " + *sql);
    }
    literal = "struct_pack(" + Join(fields, ", ") + ")";
  } else if (value.type()->IsDouble() && !std::isfinite(value.double_value())) {
    literal = QuoteLiteral(std::isnan(value.double_value()) ? "nan"
                           : value.double_value() < 0       ? "-inf"
                                                            : "inf");
  } else {
    literal = value.GetSQLLiteral(GoogleSqlLanguageOptions());
    if (value.type()->IsDate() || value.type()->IsTimestamp() || value.type()->IsDatetime() ||
        value.type()->IsTime() || value.type()->IsNumericType() ||
        value.type()->IsBigNumericType() || value.type()->IsJson()) {
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
