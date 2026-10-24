#include "src/udf/javascript/engine.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "quickjs.h"
#include "src/udf/javascript/limits.h"

namespace bigquery_emulator_duckdb::udf::javascript {

Engine::Engine(const JavaScriptLimits& limits)
    : limits_(limits), runtime_(NewRuntime(limits, this)), context_(JS_NewContext(runtime_)) {}

Engine::~Engine() {
  for (auto& [source, function] : functions_) {
    JS_FreeValue(context_, function);
  }
  JS_FreeContext(context_);
  JS_FreeRuntime(runtime_);
}

absl::StatusOr<JSValue> Engine::Function(const std::string& source) {
  if (const auto found = functions_.find(source); found != functions_.end()) {
    return found->second;
  }
  Start();
  JSValue function =
      // QuickJS defines this flag as the signed expression (0 << 0).
      // NOLINTNEXTLINE(bugprone-signed-bitwise)
      JS_Eval(context_, source.data(), source.size(), "<udf>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(function)) {
    return Exception();
  }
  // A body that closes the function expression early, such as `}); (function() {`, makes the
  // source evaluate to another function, or to something else; only the whole source is one.
  if (!JS_IsFunction(context_, function) ||
      String(function) != source.substr(1, source.size() - 2)) {
    JS_FreeValue(context_, function);
    return absl::InvalidArgumentError("JavaScript UDF body is not a function body");
  }
  functions_.emplace(source, function);
  return function;
}

absl::StatusOr<JSValue> Engine::Call(JSValue function, std::vector<JSValue>& arguments) {
  Start();
  JSValue result = JS_Call(context_, function, JS_UNDEFINED, static_cast<int>(arguments.size()),
                           arguments.data());
  for (JSValue const argument : arguments) {
    JS_FreeValue(context_, argument);
  }
  if (JS_IsException(result)) {
    return Exception();
  }
  if (!JS_IsPromise(result)) {
    return result;
  }
  JSContext* job_context = nullptr;
  while (JS_PromiseState(context_, result) == JS_PROMISE_PENDING &&
         JS_ExecutePendingJob(runtime_, &job_context) > 0) {
  }
  const JSPromiseStateEnum state = JS_PromiseState(context_, result);
  JSValue settled = JS_PromiseResult(context_, result);
  JS_FreeValue(context_, result);
  if (state == JS_PROMISE_FULFILLED) {
    return settled;
  }
  std::string const message = state == JS_PROMISE_PENDING ? "JavaScript UDF Promise never settled"
                                                          : "JavaScript UDF Promise rejected: " +
                                                                String(settled).value_or("");
  JS_FreeValue(context_, settled);
  return absl::OutOfRangeError(message);
}

std::optional<std::string> Engine::String(JSValue value) {
  size_t length = 0;
  const char* text = JS_ToCStringLen(context_, &length, value);
  if (text == nullptr) {
    JS_FreeValue(context_, JS_GetException(context_));
    return std::nullopt;
  }
  std::string result(text, length);
  JS_FreeCString(context_, text);
  return result;
}

JSRuntime* Engine::NewRuntime(const JavaScriptLimits& limits, Engine* engine) {
  JSRuntime* runtime = JS_NewRuntime();
  JS_SetMemoryLimit(runtime, limits.memory);
  JS_SetInterruptHandler(runtime, Interrupt, engine);
  return runtime;
}

void Engine::Start() {
  JS_UpdateStackTop(runtime_);
  deadline_ = std::chrono::steady_clock::now() + limits_.call_time;
  timed_out_ = false;
}

int Engine::Interrupt(JSRuntime* /*runtime*/, void* opaque) {
  auto* engine = static_cast<Engine*>(opaque);
  engine->timed_out_ = std::chrono::steady_clock::now() > engine->deadline_;
  return engine->timed_out_ ? 1 : 0;
}

absl::Status Engine::Exception() {
  if (timed_out_) {
    JS_FreeValue(context_, JS_GetException(context_));
    return absl::OutOfRangeError("JavaScript UDF timed out");
  }
  JSValue const exception = JS_GetException(context_);
  std::string const message = String(exception).value_or("JavaScript exception");
  JS_FreeValue(context_, exception);
  return absl::OutOfRangeError(message);
}

}  // namespace bigquery_emulator_duckdb::udf::javascript
