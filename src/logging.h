#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bigquery_emulator_duckdb {

// The least severe messages that logging writes. Debug messages are ABSL_VLOG(1).
enum class LogLevel : uint8_t { kDebug, kInfo, kWarning, kError };

// The level that `name`, one of debug, info, warning and error, names.
std::optional<LogLevel> ParseLogLevel(std::string_view name);

// Writes absl::log messages, GoogleSQL's included, at `level` and above as text lines to the end
// of `path`, or to standard error when `path` is empty, instead of absl's own stderr output.
// Each line holds the time, the level, and the request and job that the thread is serving.
// Call it once, before anything logs; it throws when the file cannot be opened.
void StartLogging(LogLevel level, const std::string& path);

// Marks the messages that the calling thread logs from now on as serving REST request `id`,
// until the next call. The thread serves no job at first.
void SetLogRequest(uint64_t id);

// Marks the messages that the calling thread logs while it lives as serving job `job`, as
// PROJECT:JOB_ID.
class ScopedLogJob {
 public:
  explicit ScopedLogJob(std::string job);
  ~ScopedLogJob();
  ScopedLogJob(const ScopedLogJob&) = delete;
  ScopedLogJob& operator=(const ScopedLogJob&) = delete;
  ScopedLogJob(ScopedLogJob&&) = delete;
  ScopedLogJob& operator=(ScopedLogJob&&) = delete;

 private:
  std::string previous_;
};

}  // namespace bigquery_emulator_duckdb
