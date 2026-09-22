#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace bigquery_emulator_duckdb {

// An error reported to clients in BigQuery's error response format.
class ApiError : public std::runtime_error {
 public:
  static ApiError InvalidQuery(const std::string& message) {
    return ApiError(400, "invalidQuery", message);
  }
  static ApiError Invalid(const std::string& message) { return ApiError(400, "invalid", message); }
  static ApiError NotFound(const std::string& message) {
    return ApiError(404, "notFound", message);
  }
  static ApiError Duplicate(const std::string& message) {
    return ApiError(409, "duplicate", message);
  }
  static ApiError Internal(const std::string& message) {
    return ApiError(500, "internalError", message);
  }

  int http_status() const { return http_status_; }
  const std::string& reason() const { return reason_; }

 private:
  // Private so that every error goes through one of the named factories above; `reason` and
  // `message` cannot be mixed up at a call site that never names them.
  ApiError(int http_status, std::string reason,  // NOLINT(bugprone-easily-swappable-parameters)
           const std::string& message)
      : std::runtime_error(message), http_status_(http_status), reason_(std::move(reason)) {}

  int http_status_;
  std::string reason_;
};

}  // namespace bigquery_emulator_duckdb
