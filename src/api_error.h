#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace bigquery_emulator_duckdb {

// An error reported to clients in BigQuery's error response format.
class ApiError : public std::runtime_error {
 public:
  static ApiError InvalidQuery(const std::string& message) {
    return {400, "invalidQuery", message};
  }
  static ApiError Invalid(const std::string& message) { return {400, "invalid", message}; }
  static ApiError NotFound(const std::string& message) { return {404, "notFound", message}; }
  static ApiError Duplicate(const std::string& message) { return {409, "duplicate", message}; }
  static ApiError Internal(const std::string& message) { return {500, "internalError", message}; }

  // This error with `message` in place of its own.
  [[nodiscard]] ApiError WithMessage(const std::string& message) const {
    return {http_status_, reason_, message};
  }

  [[nodiscard]] int http_status() const { return http_status_; }
  [[nodiscard]] std::string_view reason() const { return reason_; }

 private:
  // Private so that every error goes through one of the named factories above; `reason` and
  // `message` cannot be mixed up at a call site that never names them.
  ApiError(int http_status,
           std::string_view reason,  // NOLINT(bugprone-easily-swappable-parameters)
           const std::string& message)
      : std::runtime_error(message), http_status_(http_status), reason_(reason) {}

  int http_status_;
  // A string literal, so that copying an error cannot throw.
  std::string_view reason_;
};

}  // namespace bigquery_emulator_duckdb
