#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "quickjs.h"
#include "src/udf/javascript/limits.h"

namespace bigquery_emulator_duckdb::udf::javascript {

// One thread's JavaScript runtime, with source caching and execution limits.
// Function returns a borrowed value; Call takes its arguments and returns an owned value.
class Engine {
 public:
  explicit Engine(const JavaScriptLimits& limits);
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  ~Engine();

  [[nodiscard]] JSContext* context() const { return context_; }
  absl::StatusOr<JSValue> Function(const std::string& source);
  absl::StatusOr<JSValue> Call(JSValue function, std::vector<JSValue>& arguments);
  std::optional<std::string> String(JSValue value);
  // Consume the pending JavaScript exception, including a deadline interrupt.
  absl::Status Exception();

 private:
  static JSRuntime* NewRuntime(const JavaScriptLimits& limits, Engine* engine);
  void Start();
  static int Interrupt(JSRuntime* runtime, void* opaque);

  JavaScriptLimits limits_;
  JSRuntime* runtime_;
  JSContext* context_;
  std::unordered_map<std::string, JSValue> functions_;
  std::chrono::steady_clock::time_point deadline_;
  bool timed_out_ = false;
};

}  // namespace bigquery_emulator_duckdb::udf::javascript
