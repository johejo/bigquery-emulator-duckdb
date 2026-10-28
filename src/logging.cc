#include "src/logging.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "absl/base/log_severity.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "absl/log/log_sink_registry.h"
#include "absl/time/time.h"

namespace bigquery_emulator_duckdb {
namespace {

// What the calling thread serves, as SetLogRequest and ScopedLogJob mark it.
struct LogContext {
  std::optional<uint64_t> request;
  std::string job;
};

thread_local LogContext context;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

std::string_view LevelName(const absl::LogEntry& entry) {
  switch (entry.log_severity()) {
    case absl::LogSeverity::kInfo:
      return entry.verbosity() > 0 ? "DEBUG" : "INFO";
    case absl::LogSeverity::kWarning:
      return "WARNING";
    case absl::LogSeverity::kError:
      return "ERROR";
    case absl::LogSeverity::kFatal:
      return "FATAL";
  }
  return "INFO";
}

// Writes each message as one line to a file or to standard error.
class LineSink final : public absl::LogSink {
 public:
  explicit LineSink(const std::string& path) {
    if (!path.empty()) {
      file_.open(path, std::ios::app);
      if (!file_) {
        throw std::runtime_error("Cannot open log file " + path);
      }
    }
  }

  void Send(const absl::LogEntry& entry) override {
    std::string line =
        absl::FormatTime("%Y-%m-%dT%H:%M:%E6SZ ", entry.timestamp(), absl::UTCTimeZone());
    line += LevelName(entry);
    if (context.request.has_value()) {
      line += " request=" + std::to_string(*context.request);
    }
    if (!context.job.empty()) {
      line += " job=" + context.job;
    }
    line += ' ';
    line += entry.text_message();
    line += '\n';
    const std::scoped_lock lock(mutex_);
    std::ostream& out = file_.is_open() ? static_cast<std::ostream&>(file_) : std::cerr;
    out << line << std::flush;
  }

 private:
  std::mutex mutex_;
  std::ofstream file_;
};

}  // namespace

std::optional<LogLevel> ParseLogLevel(std::string_view name) {
  if (name == "debug") {
    return LogLevel::kDebug;
  }
  if (name == "info") {
    return LogLevel::kInfo;
  }
  if (name == "warning") {
    return LogLevel::kWarning;
  }
  if (name == "error") {
    return LogLevel::kError;
  }
  return std::nullopt;
}

void StartLogging(LogLevel level, const std::string& path) {
  // Threads may log until the process exits, so the sink is never destroyed.
  static auto* const sink = new LineSink(path);  // NOLINT(cppcoreguidelines-owning-memory)
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfinity);
  switch (level) {
    case LogLevel::kDebug:
      absl::SetGlobalVLogLevel(1);
      absl::SetMinLogLevel(absl::LogSeverityAtLeast::kInfo);
      break;
    case LogLevel::kInfo:
      absl::SetMinLogLevel(absl::LogSeverityAtLeast::kInfo);
      break;
    case LogLevel::kWarning:
      absl::SetMinLogLevel(absl::LogSeverityAtLeast::kWarning);
      break;
    case LogLevel::kError:
      absl::SetMinLogLevel(absl::LogSeverityAtLeast::kError);
      break;
  }
  absl::AddLogSink(sink);
}

void SetLogRequest(uint64_t id) { context = LogContext{.request = id, .job = {}}; }

ScopedLogJob::ScopedLogJob(std::string job)
    : previous_(std::exchange(context.job, std::move(job))) {}

ScopedLogJob::~ScopedLogJob() { context.job = std::move(previous_); }

}  // namespace bigquery_emulator_duckdb
