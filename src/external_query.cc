#include "src/external_query.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_split.h"
#include "libpq-fe.h"
#include "nlohmann/json.hpp"
#include "postgres_ext.h"
#include "src/connection_id.h"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {
namespace {

// The BigQuery type of a column of the PostgreSQL type `oid`, after the mapping BigQuery
// documents, or an error status for a type it or the emulator rejects. A domain's column has the
// domain's base type in a query's result, so it needs no lookup.
absl::StatusOr<FieldType> ColumnType(Oid oid, FieldType decimal_type) {
  switch (oid) {
    case 16:  // boolean
      return FieldType::kBoolean;
    case 20:  // bigint
    case 21:  // smallint
    case 23:  // integer
      return FieldType::kInteger;
    case 700:  // real
    case 701:  // double precision
      return FieldType::kFloat;
    case 1700:  // numeric
      return decimal_type;
    case 1082:
      return FieldType::kDate;
    case 1083:  // time without time zone
      return FieldType::kTime;
    case 1114:  // timestamp without time zone
      return FieldType::kDatetime;
    case 1184:  // timestamp with time zone
      return FieldType::kTimestamp;
    case 25:    // text
    case 114:   // json
    case 142:   // xml
    case 1042:  // character
    case 1043:  // character varying
    // name, which the information_schema views that BigQuery's documented metadata queries read
    // are made of.
    case 19:
      return FieldType::kString;
    case 17:  // bytea
      return FieldType::kBytes;
    // The types BigQuery lists as unsupported: money, time with time zone, interval, inet, cidr,
    // path, pg_lsn, point, polygon, box, circle, line, lseg, tsquery, tsvector, txid_snapshot,
    // uuid, jsonb, macaddr and macaddr8.
    case 790:
    case 1266:
    case 1186:
    case 869:
    case 650:
    case 602:
    case 3220:
    case 600:
    case 604:
    case 603:
    case 718:
    case 628:
    case 601:
    case 3615:
    case 3614:
    case 2970:
    case 2950:
    case 3802:
    case 829:
    case 774:
      return absl::InvalidArgumentError("Postgres type (OID = " + std::to_string(oid) +
                                        ") isn't supported now");
    default:
      // Including bit and bit varying, which BigQuery reads as BYTES in a form it does not
      // document.
      return absl::InvalidArgumentError(
          "The emulator does not support EXTERNAL_QUERY Postgres type (OID = " +
          std::to_string(oid) + ")");
  }
}

// libpq's messages end with a newline.
std::string Trimmed(const char* message) {
  return std::string(absl::StripTrailingAsciiWhitespace(message == nullptr ? "" : message));
}

}  // namespace

ExternalConnection ParseConnectionArgument(std::string_view argument) {
  const size_t equals = argument.find('=');
  const std::optional<std::string> key = equals == std::string_view::npos
                                             ? std::nullopt
                                             : ConnectionKey(argument.substr(0, equals), "");
  if (!key.has_value() || equals + 1 == argument.size()) {
    throw std::invalid_argument(
        "--connection requires PROJECT.LOCATION.CONNECTION=CONNINFO, such as "
        "my-project.us.my-connection=postgresql://user@localhost/database");
  }
  const std::string conninfo(argument.substr(equals + 1));
  if (conninfo.starts_with("spanner://")) {
    const std::vector<std::string> path = absl::StrSplit(conninfo.substr(10), '/');
    if (path.size() != 6 || path.at(0) != "projects" || path.at(1).empty() ||
        path.at(2) != "instances" || path.at(3).empty() || path.at(4) != "databases" ||
        path.at(5).empty()) {
      throw std::invalid_argument(
          "Spanner connection requires "
          "spanner://projects/PROJECT/instances/INSTANCE/databases/DATABASE");
    }
  }
  return ExternalConnection{.key = *key, .conninfo = conninfo};
}

std::string ExternalDatabaseName(const std::string& key) { return "external_query/" + key; }

absl::StatusOr<FieldType> DecimalColumnType(const std::string& options) {
  const nlohmann::json parsed = nlohmann::json::parse(options, nullptr, /*allow_exceptions=*/false);
  if (!parsed.is_object()) {
    return absl::InvalidArgumentError("options must be a JSON object, but got: " + options);
  }
  FieldType type = FieldType::kNumeric;
  for (const auto& [name, value] : parsed.items()) {
    if (name != "default_type_for_decimal_columns") {
      return absl::InvalidArgumentError("The emulator does not support EXTERNAL_QUERY option " +
                                        name);
    }
    const std::string text = value.is_string() ? value.get<std::string>() : value.dump();
    if (text == "float64") {
      type = FieldType::kFloat;
    } else if (text == "numeric") {
      type = FieldType::kNumeric;
    } else if (text == "bignumeric") {
      type = FieldType::kBigNumeric;
    } else if (text == "string") {
      type = FieldType::kString;
    } else {
      return absl::InvalidArgumentError(
          "default_type_for_decimal_columns must be float64, numeric, bignumeric or string, but "
          "got: " +
          text);
    }
  }
  return type;
}

absl::StatusOr<std::vector<FieldSchema>> DescribePostgresQuery(const std::string& conninfo,
                                                               const std::string& query,
                                                               FieldType decimal_type) {
  const std::unique_ptr<PGconn, decltype(&PQfinish)> connection(PQconnectdb(conninfo.c_str()),
                                                                PQfinish);
  if (PQstatus(connection.get()) != CONNECTION_OK) {
    return absl::InvalidArgumentError("Connect to PostgreSQL server failed: " +
                                      Trimmed(PQerrorMessage(connection.get())));
  }
  const std::unique_ptr<PGresult, decltype(&PQclear)> prepared(
      PQprepare(connection.get(), "", query.c_str(), 0, nullptr), PQclear);
  if (PQresultStatus(prepared.get()) != PGRES_COMMAND_OK) {
    const char* primary = PQresultErrorField(prepared.get(), PG_DIAG_MESSAGE_PRIMARY);
    return absl::InvalidArgumentError(
        primary != nullptr ? std::string(primary) : Trimmed(PQerrorMessage(connection.get())));
  }
  const std::unique_ptr<PGresult, decltype(&PQclear)> described(
      PQdescribePrepared(connection.get(), ""), PQclear);
  if (PQresultStatus(described.get()) != PGRES_COMMAND_OK) {
    return absl::InvalidArgumentError(Trimmed(PQerrorMessage(connection.get())));
  }
  if (PQnfields(described.get()) == 0) {
    return absl::InvalidArgumentError(
        "The emulator does not support EXTERNAL_QUERY of a statement "
        "that returns no columns");
  }
  std::vector<FieldSchema> columns;
  std::set<std::string> names;
  for (int i = 0; i < PQnfields(described.get()); ++i) {
    const absl::StatusOr<FieldType> type = ColumnType(PQftype(described.get(), i), decimal_type);
    if (!type.ok()) {
      return type.status();
    }
    std::string name = PQfname(described.get(), i);
    // postgres_scanner reads the columns of the query by name.
    if (!names.insert(absl::AsciiStrToLower(name)).second) {
      return absl::InvalidArgumentError(
          "The emulator does not support EXTERNAL_QUERY columns of the same name: " + name);
    }
    columns.push_back(FieldSchema{.name = std::move(name), .type = *type});
  }
  return columns;
}

}  // namespace bigquery_emulator_duckdb
