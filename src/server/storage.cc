#include "src/server/storage.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "google/cloud/bigquery/storage/v1/storage.grpc.pb.h"
#include "google/cloud/bigquery/storage/v1/storage.pb.h"
#include "google/cloud/bigquery/storage/v1/stream.pb.h"
#include "google/cloud/bigquery/storage/v1/table.pb.h"
#include "google/protobuf/timestamp.pb.h"
#include "grpc/impl/channel_arg_names.h"
#include "grpcpp/security/server_credentials.h"
#include "grpcpp/server.h"
#include "grpcpp/server_builder.h"
#include "grpcpp/server_context.h"
#include "grpcpp/support/status.h"
#include "grpcpp/support/sync_stream.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/duckdb_sql.h"
#include "src/emulator.h"
#include "src/field_schema.h"
#include "src/references.h"
#include "src/server/storage_codec.h"

namespace bigquery_emulator_duckdb {
namespace {

namespace pb = google::cloud::bigquery::storage::v1;
using nlohmann::json;
using Clock = std::chrono::system_clock;

class RpcError : public std::runtime_error {
 public:
  RpcError(grpc::StatusCode code, const std::string& message)
      : std::runtime_error(message), code_(code) {}
  [[nodiscard]] grpc::Status status() const { return {code_, what()}; }

 private:
  grpc::StatusCode code_;
};

[[noreturn]] void Fail(grpc::StatusCode code, const std::string& message) {
  throw RpcError(code, message);
}

template <typename F>
grpc::Status Guard(F&& body) {
  try {
    std::forward<F>(body)();
    return grpc::Status::OK;
  } catch (const RpcError& error) {
    return error.status();
  } catch (const ApiError& error) {
    const std::string message = error.what();
    const grpc::StatusCode code = error.http_status() == 404   ? grpc::StatusCode::NOT_FOUND
                                  : error.http_status() == 409 ? grpc::StatusCode::ALREADY_EXISTS
                                  : error.http_status() >= 500 ? grpc::StatusCode::INTERNAL
                                  : message.find("does not support") != std::string::npos
                                      ? grpc::StatusCode::UNIMPLEMENTED
                                      : grpc::StatusCode::INVALID_ARGUMENT;
    return {code, message};
  } catch (const std::exception& error) {
    return {grpc::StatusCode::INTERNAL, error.what()};
  }
}

std::vector<std::string> Parts(std::string_view name) {
  std::vector<std::string> parts;
  while (true) {
    const size_t slash = name.find('/');
    const auto part = name.substr(0, slash);
    if (part.empty()) {
      Fail(grpc::StatusCode::INVALID_ARGUMENT, "Invalid resource name");
    }
    parts.emplace_back(part);
    if (slash == std::string_view::npos) {
      break;
    }
    name.remove_prefix(slash + 1);
  }
  return parts;
}

TableReference ParseTable(const Emulator& emulator, std::string_view name) {
  const auto parts = Parts(name);
  if (parts.size() != 6 || parts.at(0) != "projects" || parts.at(2) != "datasets" ||
      parts.at(4) != "tables") {
    Fail(grpc::StatusCode::INVALID_ARGUMENT,
         "Expected projects/{project}/datasets/{dataset}/tables/{table}");
  }
  return {
      .project_id = emulator.ResolveProject(parts.at(1)),
      .dataset_id = parts.at(3),
      .table_id = parts.at(5),
  };
}

std::string TablePath(const TableReference& table) {
  return "projects/" + table.project_id + "/datasets/" + table.dataset_id + "/tables/" +
         table.table_id;
}

void Timestamp(google::protobuf::Timestamp* out, Clock::time_point time) {
  const auto micros =
      std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
  out->set_seconds(micros / 1000000);
  out->set_nanos(static_cast<int32_t>((micros % 1000000) * 1000));
}

std::string NewId() {
  std::random_device random;
  std::string result;
  constexpr char hex[] = "0123456789abcdef";
  for (int i = 0; i < 4; ++i) {
    const auto word = random();
    for (unsigned shift = 0; shift < 32; shift += 4) {
      result += hex[(word >> shift) & 15U];
    }
  }
  return result;
}

// Field projections retain the table's field order, including inside repeated STRUCTs.
struct Projection {
  FieldSchema field;
  size_t index = 0;
  std::vector<Projection> children;
};

std::vector<Projection> Select(const std::vector<FieldSchema>& schema,
                               const std::vector<std::string>& paths) {
  std::vector<Projection> result;
  std::vector<bool> matched(paths.size(), false);
  for (size_t i = 0; i < schema.size(); ++i) {
    bool whole = paths.empty();
    std::vector<std::string> children;
    for (size_t j = 0; j < paths.size(); ++j) {
      const auto dot = paths.at(j).find('.');
      if (ToLowerAscii(paths.at(j).substr(0, dot)) == ToLowerAscii(schema.at(i).name)) {
        matched.at(j) = true;
        if (dot == std::string::npos) {
          whole = true;
        } else {
          children.push_back(paths.at(j).substr(dot + 1));
        }
      }
    }
    if (!whole && children.empty()) {
      continue;
    }
    Projection projection{.field = schema.at(i), .index = i, .children = {}};
    if (!children.empty() && schema.at(i).type != FieldType::kRecord) {
      Fail(grpc::StatusCode::INVALID_ARGUMENT,
           "Cannot select a nested field of " + schema.at(i).name);
    }
    if (schema.at(i).type == FieldType::kRecord) {
      if (whole && !children.empty()) {
        (void)Select(schema.at(i).fields, children);
      }
      projection.children =
          Select(schema.at(i).fields, whole ? std::vector<std::string>{} : children);
      projection.field.fields.clear();
      for (const auto& child : projection.children) {
        projection.field.fields.push_back(child.field);
      }
    }
    result.push_back(std::move(projection));
  }
  for (size_t j = 0; j < paths.size(); ++j) {
    if (!matched.at(j)) {
      Fail(grpc::StatusCode::INVALID_ARGUMENT, "Unknown selected field: " + paths.at(j));
    }
  }
  return result;
}

json ProjectRecord(const json& value, const std::vector<Projection>& fields);

json ProjectCell(const json& cell, const Projection& field) {
  if (field.field.type != FieldType::kRecord || cell.at("v").is_null()) {
    return cell;
  }
  if (field.field.mode == FieldMode::kRepeated) {
    json result = json::array();
    for (const auto& item : cell.at("v")) {
      result.push_back({{"v", ProjectRecord(item.at("v"), field.children)}});
    }
    return {{"v", std::move(result)}};
  }
  return {{"v", ProjectRecord(cell.at("v"), field.children)}};
}

json ProjectRecord(const json& value, const std::vector<Projection>& fields) {
  json cells = json::array();
  for (const auto& field : fields) {
    cells.push_back(ProjectCell(value.at("f").at(field.index), field));
  }
  return {{"f", std::move(cells)}};
}

struct Snapshot {
  QueryResult data;
  std::string schema;
  bool arrow = false;
  Clock::time_point expiry;
};

struct ReadStream {
  std::shared_ptr<const Snapshot> snapshot;
  size_t begin = 0;
  size_t end = 0;
};

class ReadService final : public pb::BigQueryRead::Service {
 public:
  explicit ReadService(Emulator& emulator) : emulator_(emulator) {}

  grpc::Status CreateReadSession(grpc::ServerContext* /*context*/,
                                 const pb::CreateReadSessionRequest* request,
                                 pb::ReadSession* response) override {
    return Guard([&] {
      const auto parent = Parts(request->parent());
      if (parent.size() != 2 || parent.at(0) != "projects") {
        Fail(grpc::StatusCode::INVALID_ARGUMENT, "Expected projects/{project}");
      }
      const std::string project = emulator_.ResolveProject(parent.at(1));
      const auto& session = request->read_session();
      if (session.data_format() != pb::ARROW && session.data_format() != pb::AVRO) {
        Fail(grpc::StatusCode::INVALID_ARGUMENT, "data_format must be ARROW or AVRO");
      }
      if (request->max_stream_count() < 0 || request->max_stream_count() > 1000 ||
          request->preferred_min_stream_count() < 0 ||
          request->preferred_min_stream_count() > 1000 ||
          (request->max_stream_count() != 0 &&
           request->preferred_min_stream_count() > request->max_stream_count())) {
        Fail(grpc::StatusCode::INVALID_ARGUMENT, "Invalid read stream count");
      }
      if (session.trace_id().size() > 256) {
        Fail(grpc::StatusCode::INVALID_ARGUMENT, "trace_id exceeds 256 bytes");
      }
      if (session.table_modifiers().has_snapshot_time()) {
        Fail(grpc::StatusCode::UNIMPLEMENTED,
             "Storage Read does not support historical snapshot_time");
      }
      const auto& options = session.read_options();
      if ((session.data_format() == pb::ARROW && options.has_avro_serialization_options()) ||
          (session.data_format() == pb::AVRO && options.has_arrow_serialization_options())) {
        Fail(grpc::StatusCode::INVALID_ARGUMENT, "Serialization options do not match data_format");
      }
      if (options.arrow_serialization_options().picos_timestamp_precision() != 0 ||
          options.avro_serialization_options().picos_timestamp_precision() != 0) {
        Fail(grpc::StatusCode::UNIMPLEMENTED,
             "Storage Read does not support timestamp precision options");
      }
      if (options.has_sample_percentage() || options.response_compression_codec() != 0 ||
          options.arrow_serialization_options().buffer_compression() != 0 ||
          options.avro_serialization_options().enable_display_name_attribute()) {
        Fail(grpc::StatusCode::UNIMPLEMENTED,
             "Storage Read does not support sampling, compression or Avro display names");
      }
      const auto table = ParseTable(emulator_, session.table());
      const auto snapshot = std::make_shared<Snapshot>();
      snapshot->data = emulator_.ReadStorageTable(table, options.row_restriction());
      const auto projection = Select(snapshot->data.schema, {options.selected_fields().begin(),
                                                             options.selected_fields().end()});
      if (!options.selected_fields().empty()) {
        snapshot->data.schema.clear();
        for (const auto& field : projection) {
          snapshot->data.schema.push_back(field.field);
        }
        std::ranges::transform(snapshot->data.rows, snapshot->data.rows.begin(),
                               [&](const auto& row) { return ProjectRecord(row, projection); });
      }
      snapshot->arrow = session.data_format() == pb::ARROW;
      snapshot->schema = StorageReadSchema(snapshot->data.schema, snapshot->arrow);
      snapshot->expiry = Clock::now() + std::chrono::hours(6);
      response->set_name("projects/" + project + "/locations/" + kLocation + "/sessions/" +
                         NewId());
      response->set_table(TablePath(table));
      response->set_data_format(session.data_format());
      *response->mutable_read_options() = options;
      response->set_trace_id(session.trace_id());
      Timestamp(response->mutable_expire_time(), snapshot->expiry);
      response->set_estimated_row_count(static_cast<int64_t>(snapshot->data.rows.size()));
      if (snapshot->arrow) {
        response->mutable_arrow_schema()->set_serialized_schema(snapshot->schema);
      } else {
        response->mutable_avro_schema()->set_schema(snapshot->schema);
      }
      const size_t count = std::min<size_t>(std::max(1, request->max_stream_count()),
                                            std::max<size_t>(1, snapshot->data.rows.size()));
      std::scoped_lock const lock(mutex_);
      std::erase_if(streams_, [](const auto& stream) {
        return stream.second.snapshot->expiry <= Clock::now();
      });
      for (size_t i = 0; i < count; ++i) {
        const std::string name = response->name() + "/streams/" + std::to_string(i);
        response->add_streams()->set_name(name);
        streams_.emplace(name, ReadStream{
                                   .snapshot = snapshot,
                                   .begin = snapshot->data.rows.size() * i / count,
                                   .end = snapshot->data.rows.size() * (i + 1) / count,
                               });
      }
    });
  }

  grpc::Status ReadRows(grpc::ServerContext* context, const pb::ReadRowsRequest* request,
                        grpc::ServerWriter<pb::ReadRowsResponse>* writer) override {
    return Guard([&] {
      ReadStream stream;
      {
        std::scoped_lock const lock(mutex_);
        stream = Lookup(request->read_stream());
      }
      if (request->has_arrow_serialization_options()) {
        Fail(grpc::StatusCode::UNIMPLEMENTED,
             "Storage Read does not support ReadRows serialization options");
      }
      const size_t length = stream.end - stream.begin;
      if (request->offset() < 0 || std::cmp_greater(request->offset(), length)) {
        Fail(grpc::StatusCode::OUT_OF_RANGE, "Read offset is outside the stream");
      }
      size_t position = stream.begin + static_cast<size_t>(request->offset());
      bool first = true;
      while (first || position < stream.end) {
        if (context->IsCancelled()) {
          Fail(grpc::StatusCode::CANCELLED, "ReadRows cancelled");
        }
        size_t count = std::min<size_t>(1024, stream.end - position);
        std::string bytes;
        while (true) {
          bytes = StorageReadRows(
              stream.snapshot->data.schema,
              std::span<const json>(stream.snapshot->data.rows).subspan(position, count),
              stream.snapshot->arrow);
          if (bytes.size() <= size_t{4} * 1024 * 1024 || count <= 1) {
            break;
          }
          count /= 2;
        }
        if (bytes.size() > size_t{128} * 1024 * 1024) {
          Fail(grpc::StatusCode::RESOURCE_EXHAUSTED, "A row exceeds the 128 MB ReadRows limit");
        }
        pb::ReadRowsResponse response;
        response.set_row_count(static_cast<int64_t>(count));
        if (stream.snapshot->arrow) {
          response.mutable_arrow_record_batch()->set_serialized_record_batch(std::move(bytes));
          if (first) {
            response.mutable_arrow_schema()->set_serialized_schema(stream.snapshot->schema);
          }
        } else {
          response.mutable_avro_rows()->set_serialized_binary_rows(std::move(bytes));
          if (first) {
            response.mutable_avro_schema()->set_schema(stream.snapshot->schema);
          }
        }
        auto* progress = response.mutable_stats()->mutable_progress();
        progress->set_at_response_start(length == 0 ? 1
                                                    : static_cast<double>(position - stream.begin) /
                                                          static_cast<double>(length));
        progress->set_at_response_end(length == 0
                                          ? 1
                                          : static_cast<double>(position + count - stream.begin) /
                                                static_cast<double>(length));
        response.mutable_throttle_state()->set_throttle_percent(0);
        if (!writer->Write(response)) {
          Fail(grpc::StatusCode::CANCELLED, "ReadRows closed");
        }
        first = false;
        position += count;
      }
    });
  }

  grpc::Status SplitReadStream(grpc::ServerContext* /*context*/,
                               const pb::SplitReadStreamRequest* request,
                               pb::SplitReadStreamResponse* response) override {
    return Guard([&] {
      if (!std::isfinite(request->fraction()) || request->fraction() <= 0 ||
          request->fraction() >= 1) {
        Fail(grpc::StatusCode::INVALID_ARGUMENT, "Split fraction must be between zero and one");
      }
      std::scoped_lock const lock(mutex_);
      const ReadStream stream = Lookup(request->name());
      const size_t length = stream.end - stream.begin;
      if (length < 2) {
        return;
      }
      const size_t middle =
          stream.begin +
          std::clamp<size_t>(static_cast<size_t>(static_cast<double>(length) * request->fraction()),
                             1, length - 1);
      const std::string parent = request->name().substr(0, request->name().rfind('/') + 1);
      const std::string primary = parent + NewId();
      const std::string remainder = parent + NewId();
      streams_.emplace(
          primary, ReadStream{.snapshot = stream.snapshot, .begin = stream.begin, .end = middle});
      streams_.emplace(remainder,
                       ReadStream{.snapshot = stream.snapshot, .begin = middle, .end = stream.end});
      response->mutable_primary_stream()->set_name(primary);
      response->mutable_remainder_stream()->set_name(remainder);
    });
  }

 private:
  ReadStream Lookup(const std::string& name) {
    const auto stream = streams_.find(name);
    if (stream == streams_.end() || stream->second.snapshot->expiry <= Clock::now()) {
      Fail(grpc::StatusCode::NOT_FOUND, "Read stream not found or expired: " + name);
    }
    return stream->second;
  }
  Emulator& emulator_;
  std::mutex mutex_;
  std::unordered_map<std::string, ReadStream> streams_;
};

pb::TableFieldSchema::Type StorageType(FieldType type) {
  switch (type) {
    case FieldType::kString:
      return pb::TableFieldSchema::STRING;
    case FieldType::kBytes:
      return pb::TableFieldSchema::BYTES;
    case FieldType::kInteger:
      return pb::TableFieldSchema::INT64;
    case FieldType::kFloat:
      return pb::TableFieldSchema::DOUBLE;
    case FieldType::kBoolean:
      return pb::TableFieldSchema::BOOL;
    case FieldType::kNumeric:
      return pb::TableFieldSchema::NUMERIC;
    case FieldType::kBigNumeric:
      return pb::TableFieldSchema::BIGNUMERIC;
    case FieldType::kDate:
      return pb::TableFieldSchema::DATE;
    case FieldType::kTime:
      return pb::TableFieldSchema::TIME;
    case FieldType::kDatetime:
      return pb::TableFieldSchema::DATETIME;
    case FieldType::kTimestamp:
      return pb::TableFieldSchema::TIMESTAMP;
    case FieldType::kRecord:
      return pb::TableFieldSchema::STRUCT;
    case FieldType::kGeography:
      return pb::TableFieldSchema::GEOGRAPHY;
    case FieldType::kJson:
      return pb::TableFieldSchema::JSON;
    case FieldType::kInterval:
      return pb::TableFieldSchema::INTERVAL;
    case FieldType::kRange:
      return pb::TableFieldSchema::RANGE;
  }
  Fail(grpc::StatusCode::INTERNAL, "Unknown table field type");
}

void StorageField(pb::TableFieldSchema* out, const FieldSchema& field) {
  out->set_name(field.name);
  out->set_type(StorageType(field.type));
  out->set_mode(field.mode == FieldMode::kRepeated   ? pb::TableFieldSchema::REPEATED
                : field.mode == FieldMode::kRequired ? pb::TableFieldSchema::REQUIRED
                                                     : pb::TableFieldSchema::NULLABLE);
  out->set_description(field.description);
  if (field.max_length) {
    out->set_max_length(*field.max_length);
  }
  if (field.precision) {
    out->set_precision(*field.precision);
  }
  if (field.scale) {
    out->set_scale(*field.scale);
  }
  out->set_default_value_expression(field.default_value_expression);
  if (field.range_element_type) {
    out->mutable_range_element_type()->set_type(StorageType(*field.range_element_type));
  }
  for (const auto& child : field.fields) {
    StorageField(out->add_fields(), child);
  }
}

struct WriteState {
  pb::WriteStream metadata;
  TableReference table;
  std::vector<FieldSchema> schema;
  int64_t storage_id = 0;
  int64_t count = 0;
  int64_t published = 0;
  json staged = json::array();
  bool finalized = false;
  bool active = false;
};

class WriteService final : public pb::BigQueryWrite::Service {
 public:
  explicit WriteService(Emulator& emulator) : emulator_(emulator) {}

  grpc::Status CreateWriteStream(grpc::ServerContext* /*context*/,
                                 const pb::CreateWriteStreamRequest* request,
                                 pb::WriteStream* response) override {
    return Guard([&] {
      const auto type = request->write_stream().type();
      if (type != pb::WriteStream::COMMITTED && type != pb::WriteStream::PENDING &&
          type != pb::WriteStream::BUFFERED) {
        Fail(grpc::StatusCode::INVALID_ARGUMENT,
             "Write stream type must be COMMITTED, PENDING or BUFFERED");
      }
      if (request->write_stream().write_mode() != pb::WriteStream::WRITE_MODE_UNSPECIFIED &&
          request->write_stream().write_mode() != pb::WriteStream::INSERT) {
        Fail(grpc::StatusCode::UNIMPLEMENTED, "Storage Write does not support this write_mode");
      }
      const auto table = ParseTable(emulator_, request->parent());
      const auto info = DestinationInfo(table);
      auto state = NewStream(info, TablePath(table) + "/streams/" + NewId(), type);
      *response = state->metadata;
      std::scoped_lock const lock(mutex_);
      streams_.emplace(state->metadata.name(), std::move(state));
    });
  }

  grpc::Status GetWriteStream(grpc::ServerContext* /*context*/,
                              const pb::GetWriteStreamRequest* request,
                              pb::WriteStream* response) override {
    return Guard([&] {
      if (request->view() != pb::WRITE_STREAM_VIEW_UNSPECIFIED && request->view() != pb::BASIC &&
          request->view() != pb::FULL) {
        Fail(grpc::StatusCode::INVALID_ARGUMENT, "Invalid write stream view");
      }
      std::scoped_lock const lock(mutex_);
      const auto state = Get(request->name());
      if (IsDefault(state->metadata.name()) && !state->metadata.has_create_time()) {
        Fail(grpc::StatusCode::UNIMPLEMENTED,
             "Creation time is unknown for this table from an older emulator version");
      }
      *response = state->metadata;
      if (request->view() != pb::FULL) {
        response->clear_table_schema();
      } else {
        response->mutable_table_schema()->clear_fields();
        const auto info = DestinationInfo(state->table);
        for (const auto& field : info.schema) {
          StorageField(response->mutable_table_schema()->add_fields(), field);
        }
      }
    });
  }

  grpc::Status AppendRows(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<pb::AppendRowsResponse, pb::AppendRowsRequest>* rpc) override {
    Connection connection(*this);
    pb::AppendRowsRequest request;
    while (rpc->Read(&request)) {
      pb::AppendRowsResponse response;
      const grpc::Status status = Guard([&] { Append(request, connection, response); });
      if (!status.ok()) {
        response.mutable_error()->set_code(status.error_code());
        response.mutable_error()->set_message(status.error_message());
      }
      response.set_write_stream(request.write_stream().empty() ? connection.name
                                                               : request.write_stream());
      if (!rpc->Write(response)) {
        return {grpc::StatusCode::CANCELLED, "AppendRows closed"};
      }
      request.Clear();
    }
    return context->IsCancelled()
               ? grpc::Status(grpc::StatusCode::CANCELLED, "AppendRows cancelled")
               : grpc::Status::OK;
  }

  grpc::Status FinalizeWriteStream(grpc::ServerContext* /*context*/,
                                   const pb::FinalizeWriteStreamRequest* request,
                                   pb::FinalizeWriteStreamResponse* response) override {
    return Guard([&] {
      std::scoped_lock const lock(mutex_);
      const auto state = Get(request->name());
      if (IsDefault(state->metadata.name())) {
        Fail(grpc::StatusCode::INVALID_ARGUMENT, "Cannot finalize the default stream");
      }
      state->finalized = true;
      response->set_row_count(state->count);
    });
  }

  grpc::Status FlushRows(grpc::ServerContext* /*context*/, const pb::FlushRowsRequest* request,
                         pb::FlushRowsResponse* response) override {
    return Guard([&] {
      std::scoped_lock const lock(mutex_);
      const auto state = Get(request->write_stream());
      if (state->metadata.type() != pb::WriteStream::BUFFERED) {
        Fail(grpc::StatusCode::INVALID_ARGUMENT, "FlushRows requires a BUFFERED stream");
      }
      const int64_t offset = request->has_offset() ? request->offset().value() : state->count - 1;
      if (offset < -1 || offset >= state->count || (request->has_offset() && offset < 0)) {
        Fail(grpc::StatusCode::OUT_OF_RANGE, "Flush offset is outside the stream");
      }
      if (offset >= state->published) {
        const auto size = static_cast<size_t>(offset + 1 - state->published);
        const json rows =
            json::array_t(state->staged.begin(),
                          state->staged.begin() + static_cast<json::difference_type>(size));
        const auto errors =
            emulator_.WriteStorageRows(state->table, state->schema, rows, state->storage_id);
        if (!errors.empty()) {
          Fail(grpc::StatusCode::INVALID_ARGUMENT, errors.front().message);
        }
        state->staged.erase(state->staged.begin(),
                            state->staged.begin() + static_cast<json::difference_type>(size));
        state->published = offset + 1;
      }
      response->set_offset(state->published - 1);
    });
  }

  grpc::Status BatchCommitWriteStreams(grpc::ServerContext* /*context*/,
                                       const pb::BatchCommitWriteStreamsRequest* request,
                                       pb::BatchCommitWriteStreamsResponse* response) override {
    return Guard([&] {
      const auto table = ParseTable(emulator_, request->parent());
      const auto info = DestinationInfo(table);
      if (request->write_streams().empty()) {
        Fail(grpc::StatusCode::INVALID_ARGUMENT, "write_streams must not be empty");
      }
      std::scoped_lock const lock(mutex_);
      std::vector<std::shared_ptr<WriteState>> states;
      std::unordered_map<std::string, bool> seen;
      for (const auto& name : request->write_streams()) {
        std::shared_ptr<WriteState> state;
        const auto status = Guard([&] { state = Get(name); });
        auto code = pb::StorageError::STORAGE_ERROR_CODE_UNSPECIFIED;
        std::string message;
        if (!status.ok()) {
          code = pb::StorageError::STREAM_NOT_FOUND;
          message = status.error_message();
        } else if (TablePath(state->table) != TablePath(table)) {
          code = pb::StorageError::INVALID_STREAM_STATE;
          message = "Stream belongs to another table";
        } else if (state->metadata.type() != pb::WriteStream::PENDING) {
          code = pb::StorageError::INVALID_STREAM_TYPE;
          message = "Only PENDING streams can be committed";
        } else if (state->metadata.has_commit_time()) {
          code = pb::StorageError::STREAM_ALREADY_COMMITTED;
          message = "Stream was already committed";
        } else if (!state->finalized) {
          code = pb::StorageError::INVALID_STREAM_STATE;
          message = "Stream must be finalized before commit";
        }
        if (!seen.emplace(state ? state->metadata.name() : name, true).second) {
          Fail(grpc::StatusCode::INVALID_ARGUMENT, "Duplicate stream in commit request");
        }
        if (code != pb::StorageError::STORAGE_ERROR_CODE_UNSPECIFIED) {
          auto* error = response->add_stream_errors();
          error->set_entity(name);
          error->set_code(code);
          error->set_error_message(message);
        } else {
          states.push_back(std::move(state));
        }
      }
      if (!response->stream_errors().empty()) {
        return;
      }
      json rows = json::array();
      for (const auto& state : states) {
        if (SchemaToJson(state->schema) != SchemaToJson(info.schema)) {
          Fail(grpc::StatusCode::FAILED_PRECONDITION, "The table schema changed before commit");
        }
        for (const auto& row : state->staged) {
          rows.push_back(row);
        }
      }
      const auto errors = emulator_.WriteStorageRows(table, info.schema, rows, info.storage_id);
      if (!errors.empty()) {
        Fail(grpc::StatusCode::INVALID_ARGUMENT, errors.front().message);
      }
      Timestamp(response->mutable_commit_time(), Clock::now());
      for (const auto& state : states) {
        *state->metadata.mutable_commit_time() = response->commit_time();
        state->published = state->count;
        state->staged.clear();
      }
    });
  }

 private:
  static bool IsDefault(std::string_view name) { return name.ends_with("/streams/_default"); }

  struct Connection {
    explicit Connection(WriteService& owner) : service(owner) {}
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection() {
      if (lease) {
        std::scoped_lock const lock(service.mutex_);
        lease->active = false;
      }
    }
    WriteService& service;
    std::string name;
    std::string descriptor;
    int64_t storage_id = 0;
    std::unique_ptr<StorageProtoDecoder> decoder;
    std::shared_ptr<WriteState> lease;
  };

  TableInfo DestinationInfo(const TableReference& table) {
    TableInfo info = emulator_.GetTable(table, false);
    if (info.view_query) {
      Fail(grpc::StatusCode::INVALID_ARGUMENT, "Cannot write to a logical view");
    }
    return info;
  }

  static std::shared_ptr<WriteState> NewStream(const TableInfo& info, const std::string& name,
                                               pb::WriteStream::Type type) {
    auto state = std::make_shared<WriteState>();
    state->table = info.reference;
    state->schema = info.schema;
    state->storage_id = info.storage_id;
    state->metadata.set_name(name);
    state->metadata.set_type(type);
    state->metadata.set_write_mode(pb::WriteStream::INSERT);
    state->metadata.set_location(kLocation);
    if (!IsDefault(name)) {
      Timestamp(state->metadata.mutable_create_time(), Clock::now());
    } else if (info.metadata.creation_time != 0) {
      Timestamp(state->metadata.mutable_create_time(),
                Clock::time_point(std::chrono::milliseconds(info.metadata.creation_time)));
    }
    if (type == pb::WriteStream::COMMITTED && state->metadata.has_create_time()) {
      *state->metadata.mutable_commit_time() = state->metadata.create_time();
    }
    for (const auto& field : info.schema) {
      StorageField(state->metadata.mutable_table_schema()->add_fields(), field);
    }
    return state;
  }

  // Caller holds mutex_. Re-check destination identity so an old stream cannot write into a
  // new table that reused the same name, and a recreated table gets a fresh default stream.
  std::shared_ptr<WriteState> Get(const std::string& name) {
    const auto parts = Parts(name);
    if (parts.size() != 8 || parts.at(6) != "streams") {
      Fail(grpc::StatusCode::INVALID_ARGUMENT, "Invalid write stream name");
    }
    const auto table = ParseTable(emulator_, name.substr(0, name.rfind("/streams/")));
    const std::string canonical = TablePath(table) + "/streams/" + parts.at(7);
    const auto info = DestinationInfo(table);
    auto found = streams_.find(canonical);
    if (IsDefault(canonical) &&
        (found == streams_.end() || found->second->storage_id != info.storage_id)) {
      streams_[canonical] = NewStream(info, canonical, pb::WriteStream::COMMITTED);
      found = streams_.find(canonical);
    }
    if (found == streams_.end() || found->second->storage_id != info.storage_id) {
      Fail(grpc::StatusCode::NOT_FOUND, "Write stream not found: " + name);
    }
    return found->second;
  }

  void Append(const pb::AppendRowsRequest& request, Connection& connection,
              pb::AppendRowsResponse& response) {
    if (request.ByteSizeLong() >= size_t{20} * 1024 * 1024) {
      Fail(grpc::StatusCode::INVALID_ARGUMENT, "AppendRowsRequest must be smaller than 20 MB");
    }
    if (!request.has_proto_rows()) {
      Fail(grpc::StatusCode::UNIMPLEMENTED,
           "Storage Write requires protobuf rows; Arrow input is unsupported");
    }
    std::scoped_lock const lock(mutex_);
    if (connection.name.empty() && request.write_stream().empty()) {
      Fail(grpc::StatusCode::INVALID_ARGUMENT, "The first request must specify write_stream");
    }
    const auto state =
        Get(request.write_stream().empty() ? connection.name : request.write_stream());
    const std::string& name = state->metadata.name();
    const bool changing = !connection.name.empty() && connection.name != name;
    if (changing && (!IsDefault(connection.name) || !IsDefault(name))) {
      Fail(grpc::StatusCode::INVALID_ARGUMENT,
           "Only default streams can switch destinations in an AppendRows connection");
    }
    if (state->finalized) {
      Fail(grpc::StatusCode::FAILED_PRECONDITION, "Write stream is finalized");
    }
    if (IsDefault(name) && request.has_offset()) {
      Fail(grpc::StatusCode::INVALID_ARGUMENT, "Offsets are not allowed on the default stream");
    }
    if (request.has_offset()) {
      const int64_t offset = request.offset().value();
      if (offset < 0 || offset > state->count) {
        Fail(grpc::StatusCode::OUT_OF_RANGE, "Append offset is outside the stream");
      }
      if (offset < state->count) {
        Fail(grpc::StatusCode::ALREADY_EXISTS, "Append offset was already written");
      }
    }
    if (request.default_missing_value_interpretation() !=
            pb::AppendRowsRequest::MISSING_VALUE_INTERPRETATION_UNSPECIFIED &&
        request.default_missing_value_interpretation() != pb::AppendRowsRequest::NULL_VALUE &&
        request.default_missing_value_interpretation() != pb::AppendRowsRequest::DEFAULT_VALUE) {
      Fail(grpc::StatusCode::INVALID_ARGUMENT, "Invalid missing value interpretation");
    }
    for (const auto& [field, interpretation] : request.missing_value_interpretations()) {
      const bool known = std::ranges::any_of(
          state->schema, [&](const auto& entry) { return entry.name == field; });
      if (!known || (interpretation != pb::AppendRowsRequest::NULL_VALUE &&
                     interpretation != pb::AppendRowsRequest::DEFAULT_VALUE)) {
        Fail(grpc::StatusCode::INVALID_ARGUMENT,
             "Invalid missing value interpretation for " + field);
      }
    }
    const bool first = connection.name.empty() || changing;
    if (first) {
      if (!request.proto_rows().has_writer_schema()) {
        Fail(grpc::StatusCode::INVALID_ARGUMENT, "The first request must include writer_schema");
      }
      if (!IsDefault(name) && state->active) {
        Fail(grpc::StatusCode::ALREADY_EXISTS,
             "A write stream can have only one active connection");
      }
      auto decoder = std::make_unique<StorageProtoDecoder>(
          request.proto_rows().writer_schema().proto_descriptor(), state->schema);
      connection.decoder = std::move(decoder);
      connection.name = name;
      connection.storage_id = state->storage_id;
      connection.descriptor =
          request.proto_rows().writer_schema().proto_descriptor().SerializeAsString();
      if (!IsDefault(name)) {
        state->active = true;
        connection.lease = state;
      }
    } else if (request.proto_rows().has_writer_schema() &&
               request.proto_rows().writer_schema().proto_descriptor().SerializeAsString() !=
                   connection.descriptor) {
      Fail(grpc::StatusCode::UNIMPLEMENTED,
           "Storage Write does not support changing writer_schema on an existing connection");
    }
    if (connection.storage_id != state->storage_id) {
      Fail(grpc::StatusCode::NOT_FOUND, "The destination table was replaced");
    }
    json rows = json::array();
    for (int i = 0; i < request.proto_rows().rows().serialized_rows_size(); ++i) {
      try {
        rows.push_back(connection.decoder->Decode(request.proto_rows().rows().serialized_rows(i)));
      } catch (const ApiError& error) {
        auto* row = response.add_row_errors();
        row->set_index(i);
        row->set_code(pb::RowError::FIELDS_ERROR);
        row->set_message(error.what());
      }
    }
    if (!response.row_errors().empty()) {
      Fail(grpc::StatusCode::INVALID_ARGUMENT, "Invalid protobuf rows; no rows were appended");
    }
    const bool staging = state->metadata.type() != pb::WriteStream::COMMITTED;
    const auto errors =
        emulator_.WriteStorageRows(state->table, state->schema, rows, state->storage_id, staging);
    for (const auto& error : errors) {
      auto* row = response.add_row_errors();
      row->set_index(static_cast<int64_t>(error.index));
      row->set_code(pb::RowError::FIELDS_ERROR);
      row->set_message(error.message);
    }
    if (!errors.empty()) {
      Fail(grpc::StatusCode::INVALID_ARGUMENT, "Invalid rows; no rows were appended");
    }
    if (!IsDefault(name)) {
      response.mutable_append_result()->mutable_offset()->set_value(state->count);
    } else {
      (void)response.mutable_append_result();
    }
    if (staging) {
      for (auto& row : rows) {
        state->staged.push_back(std::move(row));
      }
    }
    state->count += static_cast<int64_t>(rows.size());
    if (!staging) {
      state->published = state->count;
    }
  }

  Emulator& emulator_;
  std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<WriteState>> streams_;
};

std::string Endpoint(const std::string& host, int port) {
  const std::string address = host == "0.0.0.0" ? "127.0.0.1" : host == "::" ? "::1" : host;
  return (address.find(':') == std::string::npos ? address : "[" + address + "]") + ":" +
         std::to_string(port);
}

}  // namespace

class StorageServer::Impl {
 public:
  Impl(Emulator& emulator, std::string address, int requested)
      : read(emulator), write(emulator), host(std::move(address)), requested_port(requested) {}
  ReadService read;
  WriteService write;
  std::string host;
  int requested_port;
  int port = 0;
  std::unique_ptr<grpc::Server> server;
};

StorageServer::StorageServer(Emulator& emulator, std::string host, int port)
    : impl_(std::make_unique<Impl>(emulator, std::move(host), port)) {}

StorageServer::~StorageServer() {
  Stop();
  Wait();
}

bool StorageServer::Start() {
  if (impl_->requested_port < 0 || impl_->requested_port > 65535 || impl_->server) {
    return false;
  }
  grpc::ServerBuilder builder;
  builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
  builder.SetMaxReceiveMessageSize((20 * 1024 * 1024) + 1);
  builder.SetMaxSendMessageSize((128 * 1024 * 1024) + (1024 * 1024));
  const std::string address =
      (impl_->host.find(':') == std::string::npos ? impl_->host : "[" + impl_->host + "]") + ":" +
      std::to_string(impl_->requested_port);
  builder.AddListeningPort(address, grpc::InsecureServerCredentials(), &impl_->port);
  builder.RegisterService(&impl_->read);
  builder.RegisterService(&impl_->write);
  impl_->server = builder.BuildAndStart();
  return impl_->server != nullptr;
}

void StorageServer::Stop() {
  if (impl_->server) {
    impl_->server->Shutdown(Clock::now());
  }
}

void StorageServer::Wait() {
  if (impl_->server) {
    impl_->server->Wait();
  }
}

int StorageServer::port() const { return impl_->port; }
std::string StorageServer::endpoint() const { return Endpoint(impl_->host, impl_->port); }

}  // namespace bigquery_emulator_duckdb
