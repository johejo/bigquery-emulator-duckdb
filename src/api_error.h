#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace bigquery_emulator_duckdb {

// How every message about a form the emulator rejects as unsupported starts.
inline constexpr std::string_view kUnsupported = "The emulator does not support ";

// An error reported to clients in BigQuery's error response format.
class ApiError : public std::runtime_error {
 public:
  static ApiError InvalidQuery(const std::string& message) {
    return {400, "invalidQuery", message};
  }
  static ApiError Invalid(const std::string& message) { return {400, "invalid", message}; }
  // A required field left out or empty, named as BigQuery names it, such as dataset_id.
  static ApiError Required(const std::string& field) {
    return {400, "required", "Required parameter is missing: " + field};
  }
  static ApiError NotFound(const std::string& message) { return {404, "notFound", message}; }
  static ApiError Duplicate(const std::string& message) { return {409, "duplicate", message}; }
  static ApiError Internal(const std::string& message) { return {500, "internalError", message}; }
  // An invalid or invalidQuery error saying the emulator does not support `what`.
  static ApiError Unsupported(std::string_view what) {
    return Invalid(std::string(kUnsupported).append(what));
  }
  static ApiError UnsupportedQuery(std::string_view what) {
    return InvalidQuery(std::string(kUnsupported).append(what));
  }

  // This error with `message` in place of its own.
  [[nodiscard]] ApiError WithMessage(const std::string& message) const {
    return {http_status_, reason_, message, location_};
  }

  // This error with `new_location`, or no location when it is empty, in its ErrorProto.
  [[nodiscard]] ApiError WithLocation(std::string_view new_location) const {
    return {http_status_, reason_, what(), new_location};
  }

  [[nodiscard]] int http_status() const { return http_status_; }
  [[nodiscard]] std::string_view reason() const { return reason_; }
  [[nodiscard]] const std::string& location() const { return location_; }

 private:
  // Private so that every error goes through one of the named factories above; `reason` and
  // `message` cannot be mixed up at a call site that never names them.
  ApiError(int http_status,
           std::string_view reason,  // NOLINT(bugprone-easily-swappable-parameters)
           const std::string& message, std::string_view error_location = "query")
      : std::runtime_error(message),
        http_status_(http_status),
        reason_(reason),
        location_(error_location) {}

  int http_status_;
  // A string literal supplied by the factory, with no per-error allocation.
  std::string_view reason_;
  std::string location_;
};

}  // namespace bigquery_emulator_duckdb
