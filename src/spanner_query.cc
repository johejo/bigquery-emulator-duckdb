#include "src/spanner_query.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_split.h"
#include "duckdb.h"
#include "google/cloud/grpc_options.h"
#include "google/cloud/options.h"
#include "google/cloud/spanner/bytes.h"
#include "google/cloud/spanner/client.h"
#include "google/cloud/spanner/database.h"
#include "google/cloud/spanner/date.h"
#include "google/cloud/spanner/json.h"
#include "google/cloud/spanner/numeric.h"
#include "google/cloud/spanner/options.h"
#include "google/cloud/spanner/request_priority.h"
#include "google/cloud/spanner/results.h"
#include "google/cloud/spanner/row.h"
#include "google/cloud/spanner/sql_statement.h"
#include "google/cloud/spanner/timestamp.h"
#include "google/cloud/spanner/value.h"
#include "google/protobuf/empty.pb.h"
#include "google/protobuf/timestamp.pb.h"
#include "google/spanner/v1/result_set.pb.h"
#include "google/spanner/v1/spanner.grpc.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "google/spanner/v1/transaction.pb.h"
#include "google/spanner/v1/type.pb.h"
#include "grpcpp/client_context.h"
#include "grpcpp/create_channel.h"
#include "grpcpp/security/credentials.h"
#include "grpcpp/support/status.h"
#include "nlohmann/json.hpp"
#include "src/backend_error.h"
#include "src/duckdb_handle.h"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {
namespace {

namespace spanner = google::cloud::spanner;
namespace api = google::spanner::v1;

// The public C++ RowStream does not expose result metadata (including for an empty result).
// Use the Spanner RPC for PLAN only; the public client handles streaming execution and retries.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a database and its SQL query.
absl::StatusOr<api::StructType> QuerySchema(const std::string& database, const std::string& sql) {
  const char* emulator = std::getenv("SPANNER_EMULATOR_HOST");
  const bool local = emulator != nullptr && *emulator != '\0';
  auto stub = api::Spanner::NewStub(grpc::CreateChannel(
      local ? emulator : "spanner.googleapis.com",
      local ? grpc::InsecureChannelCredentials() : grpc::GoogleDefaultCredentials()));
  api::CreateSessionRequest create;
  create.set_database(database);
  api::Session session;
  grpc::ClientContext create_context;
  create_context.AddMetadata("x-goog-request-params", "database=" + database);
  create_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
  const auto created = stub->CreateSession(&create_context, create, &session);
  if (!created.ok()) {
    return absl::InvalidArgumentError(created.error_message());
  }
  // Delete the temporary session even if a query fails or the result cannot be mapped.
  const auto cleanup = [&stub](api::Session* value) {
    api::DeleteSessionRequest request;
    request.set_name(value->name());
    google::protobuf::Empty empty;
    grpc::ClientContext context;
    context.AddMetadata("x-goog-request-params", "name=" + value->name());
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    stub->DeleteSession(&context, request, &empty);
  };
  const std::unique_ptr<api::Session, decltype(cleanup)> guard(&session, cleanup);
  api::ExecuteSqlRequest request;
  request.set_session(session.name());
  request.set_sql(sql);
  request.set_query_mode(api::ExecuteSqlRequest::PLAN);
  request.mutable_request_options()->set_priority(api::RequestOptions::PRIORITY_MEDIUM);
  request.mutable_transaction()->mutable_single_use()->mutable_read_only()->set_strong(true);
  api::ResultSet result;
  grpc::ClientContext context;
  context.AddMetadata("x-goog-request-params", "session=" + session.name());
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
  const auto status = stub->ExecuteSql(&context, request, &result);
  if (!status.ok()) {
    return absl::InvalidArgumentError(status.error_message());
  }
  return result.metadata().row_type();
}

absl::StatusOr<FieldType> ColumnType(const api::Type& type) {
  // PostgreSQL annotations have different NUMERIC/JSON semantics; leave that dialect unsupported.
  if (type.type_annotation() == api::TYPE_ANNOTATION_CODE_UNSPECIFIED) {
    switch (type.code()) {
      case api::BOOL:
        return FieldType::kBoolean;
      case api::INT64:
        return FieldType::kInteger;
      case api::FLOAT64:
        return FieldType::kFloat;
      case api::STRING:
        return FieldType::kString;
      case api::BYTES:
        return FieldType::kBytes;
      case api::DATE:
        return FieldType::kDate;
      case api::TIMESTAMP:
        return FieldType::kTimestamp;
      case api::JSON:
        return FieldType::kJson;
      case api::NUMERIC:
        return FieldType::kNumeric;
      default:
        break;
    }
  }
  return absl::InvalidArgumentError("The emulator does not support EXTERNAL_QUERY Spanner type " +
                                    type.DebugString());
}

struct BoundQuery {
  std::string database;
  std::string query;
  std::vector<FieldSchema> columns;
};

struct QueryStream {
  spanner::Client client;
  spanner::RowStream rows;
  spanner::RowStreamIterator next;

  explicit QueryStream(const BoundQuery& bound)
      : client([&bound] {
          const std::vector<std::string> path = absl::StrSplit(bound.database, '/');
          return spanner::MakeConnection(
              spanner::Database(path.at(1), path.at(3), path.at(5)),
              google::cloud::Options{}.set<google::cloud::GrpcNumChannelsOption>(1));
        }()),
        rows(client.ExecuteQuery(spanner::SqlStatement(bound.query),
                                 google::cloud::Options{}.set<spanner::RequestPriorityOption>(
                                     spanner::RequestPriority::kMedium))),
        next(rows.begin()) {}
};

template <typename T>
void Delete(void* value) {
  delete static_cast<T*>(value);
}

void Bind(duckdb_bind_info info) {
  try {
    const Value database(duckdb_bind_get_parameter(info, 0));
    const Value query(duckdb_bind_get_parameter(info, 1));
    const DuckString db(duckdb_get_varchar(database.get()));
    const DuckString sql(duckdb_get_varchar(query.get()));
    if (!db || !sql) {
      throw std::invalid_argument("Spanner database and query must not be NULL");
    }
    auto columns = DescribeSpannerQuery(db.get(), sql.get(), "{}");
    if (!columns.ok()) {
      throw std::invalid_argument(std::string(columns.status().message()));
    }
    for (const auto& column : *columns) {
      duckdb_type type = DUCKDB_TYPE_VARCHAR;
      switch (column.type) {
        case FieldType::kBoolean:
          type = DUCKDB_TYPE_BOOLEAN;
          break;
        case FieldType::kInteger:
          type = DUCKDB_TYPE_BIGINT;
          break;
        case FieldType::kFloat:
          type = DUCKDB_TYPE_DOUBLE;
          break;
        case FieldType::kDate:
          type = DUCKDB_TYPE_DATE;
          break;
        case FieldType::kTimestamp:
          type = DUCKDB_TYPE_TIMESTAMP_TZ;
          break;
        case FieldType::kBytes:
          type = DUCKDB_TYPE_BLOB;
          break;
        default:
          break;
      }
      const LogicalType logical(duckdb_create_logical_type(type));
      duckdb_bind_add_result_column(info, column.name.c_str(), logical.get());
    }
    auto bound = std::make_unique<BoundQuery>(BoundQuery{
        .database = db.get(),
        .query = sql.get(),
        .columns = std::move(*columns),
    });
    duckdb_bind_set_bind_data(info, bound.release(), Delete<BoundQuery>);
  } catch (const std::exception& error) {
    duckdb_bind_set_error(info, error.what());
  }
}

void Init(duckdb_init_info info) {
  try {
    const auto& bound = *static_cast<const BoundQuery*>(duckdb_init_get_bind_data(info));
    auto stream = std::make_unique<QueryStream>(bound);
    duckdb_init_set_init_data(info, stream.release(), Delete<QueryStream>);
    duckdb_init_set_max_threads(info, 1);
  } catch (const std::exception& error) {
    duckdb_init_set_error(info, error.what());
  }
}

template <typename T>
T Get(const spanner::Value& value) {
  auto result = value.get<T>();
  if (!result) {
    throw std::runtime_error(result.status().message());
  }
  return *std::move(result);
}

void Assign(duckdb_vector vector, idx_t row, const spanner::Value& value, FieldType type) {
  const auto assign = [&]<typename T>(auto write) {
    auto result = Get<std::optional<T>>(value);
    if (!result) {
      duckdb_vector_ensure_validity_writable(vector);
      duckdb_validity_set_row_invalid(duckdb_vector_get_validity(vector), row);
    } else {
      write(*result);
    }
  };
  const auto text = [&](const std::string& result) {
    duckdb_vector_assign_string_element_len(vector, row, result.data(), result.size());
  };
  switch (type) {
    case FieldType::kBoolean:
      assign.template operator()<bool>(
          [&](bool result) { static_cast<bool*>(duckdb_vector_get_data(vector))[row] = result; });
      break;
    case FieldType::kInteger:
      assign.template operator()<int64_t>([&](int64_t result) {
        static_cast<int64_t*>(duckdb_vector_get_data(vector))[row] = result;
      });
      break;
    case FieldType::kFloat:
      assign.template operator()<double>([&](double result) {
        static_cast<double*>(duckdb_vector_get_data(vector))[row] = result;
      });
      break;
    case FieldType::kString:
      assign.template operator()<std::string>(text);
      break;
    case FieldType::kBytes:
      assign.template operator()<spanner::Bytes>(
          [&](const spanner::Bytes& result) { text(result.get<std::string>()); });
      break;
    case FieldType::kJson:
      assign.template operator()<spanner::Json>(
          [&](const spanner::Json& result) { text(static_cast<std::string>(result)); });
      break;
    case FieldType::kNumeric:
      assign.template operator()<spanner::Numeric>(
          [&](const spanner::Numeric& result) { text(result.ToString()); });
      break;
    case FieldType::kDate:
      assign.template operator()<spanner::Date>([&](const spanner::Date& result) {
        static_cast<duckdb_date*>(duckdb_vector_get_data(vector))[row] = duckdb_to_date({
            .year = static_cast<int32_t>(result.year()),
            .month = static_cast<int8_t>(result.month()),
            .day = static_cast<int8_t>(result.day()),
        });
      });
      break;
    case FieldType::kTimestamp:
      assign.template operator()<spanner::Timestamp>([&](const spanner::Timestamp& result) {
        const auto proto = result.get<google::protobuf::Timestamp>().value();
        static_cast<duckdb_timestamp*>(duckdb_vector_get_data(vector))[row] =
            duckdb_timestamp{.micros = (proto.seconds() * 1000000) + (proto.nanos() / 1000)};
      });
      break;
    default:
      throw std::runtime_error("Unsupported Spanner result type");
  }
}

void Read(duckdb_function_info info, duckdb_data_chunk output) {
  try {
    const auto& bound = *static_cast<const BoundQuery*>(duckdb_function_get_bind_data(info));
    auto& stream = *static_cast<QueryStream*>(duckdb_function_get_init_data(info));
    idx_t count = 0;
    while (count < duckdb_vector_size() && stream.next != stream.rows.end()) {
      const auto& row = *stream.next;
      if (!row) {
        throw std::runtime_error(row.status().message());
      }
      if (row->size() != bound.columns.size()) {
        throw std::runtime_error("Spanner result schema changed during EXTERNAL_QUERY");
      }
      for (size_t column = 0; column < bound.columns.size(); ++column) {
        if (row->columns().at(column) != bound.columns.at(column).name) {
          throw std::runtime_error("Spanner result schema changed during EXTERNAL_QUERY");
        }
        Assign(duckdb_data_chunk_get_vector(output, column), count, row->values().at(column),
               bound.columns.at(column).type);
      }
      ++count;
      ++stream.next;
    }
    duckdb_data_chunk_set_size(output, count);
  } catch (const std::exception& error) {
    duckdb_function_set_error(info, error.what());
  }
}

}  // namespace

// NOLINTBEGIN(bugprone-easily-swappable-parameters): an external query and its JSON options.
absl::StatusOr<std::vector<FieldSchema>> DescribeSpannerQuery(const std::string& database,
                                                              const std::string& query,
                                                              const std::string& options) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  const auto parsed = nlohmann::json::parse(options, nullptr, false);
  if (!parsed.is_object()) {
    return absl::InvalidArgumentError("options must be a JSON object, but got: " + options);
  }
  if (!parsed.empty()) {
    return absl::InvalidArgumentError(
        "The emulator does not support EXTERNAL_QUERY Spanner options");
  }
  auto schema = QuerySchema(database, query);
  if (!schema.ok()) {
    return schema.status();
  }
  std::vector<FieldSchema> columns;
  std::set<std::string> names;
  for (const auto& field : schema->fields()) {
    auto type = ColumnType(field.type());
    if (!type.ok()) {
      return type.status();
    }
    if (!names.insert(absl::AsciiStrToLower(field.name())).second) {
      return absl::InvalidArgumentError(
          "The emulator does not support EXTERNAL_QUERY columns of the same name: " + field.name());
    }
    columns.push_back(FieldSchema{.name = field.name(), .type = *type});
  }
  if (columns.empty()) {
    return absl::InvalidArgumentError(
        "The emulator does not support EXTERNAL_QUERY of a statement that returns no columns");
  }
  return columns;
}

void RegisterSpannerQuery(duckdb_connection connection) {
  const Handle<duckdb_table_function, duckdb_destroy_table_function> function(
      duckdb_create_table_function());
  duckdb_table_function_set_name(function.get(), "bq_spanner_query");
  const LogicalType text(duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
  duckdb_table_function_add_parameter(function.get(), text.get());
  duckdb_table_function_add_parameter(function.get(), text.get());
  duckdb_table_function_set_bind(function.get(), Bind);
  duckdb_table_function_set_init(function.get(), Init);
  duckdb_table_function_set_function(function.get(), Read);
  if (duckdb_register_table_function(connection, function.get()) == DuckDBError) {
    throw BackendError("DuckDB failed to register bq_spanner_query");
  }
}

}  // namespace bigquery_emulator_duckdb
