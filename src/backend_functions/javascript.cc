#include "src/backend_functions/javascript.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "quickjs.h"
#include "src/backend_error.h"
#include "src/backend_functions/scalar.h"
#include "src/duckdb_handle.h"

// JavaScript UDFs, run by QuickJS. The translator passes the UDF as the source of a function
// expression, `(function(x, y) {body})`, followed by its arguments; see
// src/translator/function.cc. Each DuckDB thread that runs a call of a query keeps a runtime of
// its own, since a QuickJS runtime must not be used by two threads, and compiles each source once
// in it; no state outlives the query.

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

// The results a JavaScript UDF can return, each registered as a function of its own since a
// DuckDB function's result type is fixed.
enum class Result : uint8_t { kBool, kDouble, kString, kInt64 };

class Engine {
 public:
  explicit Engine(const JavaScriptLimits& limits)
      : limits_(limits), runtime_(NewRuntime(limits, this)), context_(JS_NewContext(runtime_)) {}
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  ~Engine() {
    for (auto& [source, function] : functions_) {
      JS_FreeValue(context_, function);
    }
    JS_FreeContext(context_);
    JS_FreeRuntime(runtime_);
  }

  [[nodiscard]] JSContext* context() const { return context_; }

  // The function `source` evaluates to, compiled on its first use.
  absl::StatusOr<JSValue> Function(const std::string& source) {
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

  // Calls `function` with `arguments`, which it takes ownership of, and settles a returned
  // Promise. The caller owns the result.
  absl::StatusOr<JSValue> Call(JSValue function, std::vector<JSValue>& arguments) {
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

  // The text of `value`, as String(value) spells it, or nullopt when that throws.
  std::optional<std::string> String(JSValue value) {
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

 private:
  // A runtime under `limits`, which interrupts calls of `engine` past their deadline.
  static JSRuntime* NewRuntime(const JavaScriptLimits& limits, Engine* engine) {
    JSRuntime* runtime = JS_NewRuntime();
    JS_SetMemoryLimit(runtime, limits.memory);
    JS_SetInterruptHandler(runtime, Interrupt, engine);
    return runtime;
  }

  // Starts the time limit of evaluating or calling a UDF.
  void Start() {
    JS_UpdateStackTop(runtime_);
    deadline_ = std::chrono::steady_clock::now() + limits_.call_time;
    timed_out_ = false;
  }

  // Interrupts a UDF that runs past its deadline, with an exception it cannot catch.
  static int Interrupt(JSRuntime* /*runtime*/, void* opaque) {
    auto* engine = static_cast<Engine*>(opaque);
    engine->timed_out_ = std::chrono::steady_clock::now() > engine->deadline_;
    return engine->timed_out_ ? 1 : 0;
  }

  // The pending exception as an error.
  absl::Status Exception() {
    if (timed_out_) {
      JS_FreeValue(context_, JS_GetException(context_));
      return absl::OutOfRangeError("JavaScript UDF timed out");
    }
    JSValue const exception = JS_GetException(context_);
    std::string const message = String(exception).value_or("JavaScript exception");
    JS_FreeValue(context_, exception);
    return absl::OutOfRangeError(message);
  }

  JavaScriptLimits limits_;
  JSRuntime* runtime_;
  JSContext* context_;
  std::unordered_map<std::string, JSValue> functions_;
  std::chrono::steady_clock::time_point deadline_;
  bool timed_out_ = false;
};

// Gives each DuckDB thread that runs a call of a query an engine of its own, which DuckDB deletes
// when the query ends.
void InitEngine(duckdb_init_info info) {
  const auto& limits =
      *static_cast<const JavaScriptLimits*>(duckdb_scalar_function_init_get_extra_info(info));
  duckdb_scalar_function_init_set_state(info, new Engine(limits),
                                        [](void* engine) { delete static_cast<Engine*>(engine); });
}

// Argument `column` as a JavaScript value: NULL as null, and BOOL, FLOAT64 and STRING as a
// boolean, number and string.
absl::StatusOr<JSValue> Argument(JSContext* context, const Arguments& arguments, idx_t column) {
  if (arguments.IsNull(column)) {
    return JS_NULL;
  }
  LogicalType const type(duckdb_vector_get_column_type(arguments.Vector(column)));
  switch (duckdb_get_type_id(type.get())) {
    case DUCKDB_TYPE_BOOLEAN:
      return JS_NewBool(context, arguments.Bool(column));
    case DUCKDB_TYPE_DOUBLE:
      return JS_NewFloat64(context, arguments.Double(column));
    case DUCKDB_TYPE_VARCHAR: {
      const std::string text = arguments.String(column);
      return JS_NewStringLen(context, text.data(), text.size());
    }
    default:
      return absl::UnimplementedError("The emulator does not support this JavaScript UDF argument");
  }
}

template <Result R>
using ResultType = std::conditional_t<
    R == Result::kBool, bool,
    std::conditional_t<R == Result::kDouble, double,
                       std::conditional_t<R == Result::kString, std::string, int64_t>>>;

constexpr const char* ResultName(Result result) {
  switch (result) {
    case Result::kBool:
      return "BOOL";
    case Result::kDouble:
      return "FLOAT64";
    case Result::kString:
      return "STRING";
    case Result::kInt64:
      return "INT64";
  }
  return "";
}

// The SQL value of a UDF's result: null and undefined as NULL; for STRING, any other value as
// String(value) spells it; for INT64, a number rounded half away from zero, as a FLOAT64 is cast,
// or a string spelling an integer; and otherwise only a value of the declared type.
template <Result R>
absl::StatusOr<std::optional<ResultType<R>>> Convert(Engine& engine, JSValue value) {
  JSContext* context = engine.context();
  if (JS_IsNull(value) || JS_IsUndefined(value)) {
    return std::nullopt;
  }
  absl::Status mismatch = absl::OutOfRangeError(
      std::string("JavaScript UDF returned a value that is not a ") + ResultName(R));
  if constexpr (R == Result::kBool) {
    if (!JS_IsBool(value)) {
      return mismatch;
    }
    return JS_ToBool(context, value) == 1;
  } else if constexpr (R == Result::kDouble) {
    double number = 0;
    if (!JS_IsNumber(value) || JS_ToFloat64(context, &number, value) != 0) {
      return mismatch;
    }
    return number;
  } else if constexpr (R == Result::kString) {
    auto text = engine.String(value);
    if (!text) {
      return mismatch;
    }
    return *text;
  } else {
    if (JS_IsNumber(value)) {
      double number = 0;
      if (JS_ToFloat64(context, &number, value) != 0 || std::isnan(number)) {
        return mismatch;
      }
      number = std::round(number);
      // 2^63 is the first double past INT64's range.
      if (number < -9223372036854775808.0 || number >= 9223372036854775808.0) {
        return mismatch;
      }
      return static_cast<int64_t>(number);
    }
    if (JS_IsString(value)) {
      const auto text = engine.String(value);
      if (text && !text->empty()) {
        int64_t parsed = 0;
        const char* end = text->data() + text->size();
        const auto [last, error] = std::from_chars(text->data(), end, parsed);
        if (error == std::errc() && last == end) {
          return parsed;
        }
      }
    }
    return mismatch;
  }
}

// Calls the UDF whose source is argument 0 with the remaining arguments.
template <Result R>
void JavaScriptCall(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  auto* state = static_cast<Engine*>(duckdb_scalar_function_get_state(info));
  if (state == nullptr) {
    duckdb_scalar_function_set_error(info, "JavaScript UDF called without an engine");
    return;
  }
  Engine& engine = *state;
  const idx_t columns = duckdb_data_chunk_get_column_count(input);
  EachRow(
      info, input, output,
      [&](const Arguments& arguments) -> absl::StatusOr<std::optional<ResultType<R>>> {
        const auto function = engine.Function(arguments.String(0));
        if (!function.ok()) {
          return function.status();
        }
        std::vector<JSValue> values;
        for (idx_t column = 1; column < columns; ++column) {
          auto value = Argument(engine.context(), arguments, column);
          if (!value.ok()) {
            for (JSValue const converted : values) {
              JS_FreeValue(engine.context(), converted);
            }
            return value.status();
          }
          values.push_back(*value);
        }
        const auto result = engine.Call(*function, values);
        if (!result.ok()) {
          return result.status();
        }
        auto converted = Convert<R>(engine, *result);
        JS_FreeValue(engine.context(), *result);
        return converted;
      },
      /*nulls=*/false);
}

}  // namespace

void RegisterJavaScriptFunctions(duckdb_connection connection, const JavaScriptLimits& limits) {
  const auto add = [&](const char* name, duckdb_type result, duckdb_scalar_function_t function) {
    Handle<duckdb_scalar_function, duckdb_destroy_scalar_function> const scalar(
        duckdb_create_scalar_function());
    duckdb_scalar_function_set_name(scalar.get(), name);
    LogicalType const source(duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_scalar_function_add_parameter(scalar.get(), source.get());
    LogicalType const any(duckdb_create_logical_type(DUCKDB_TYPE_ANY));
    duckdb_scalar_function_set_varargs(scalar.get(), any.get());
    LogicalType const type(duckdb_create_logical_type(result));
    duckdb_scalar_function_set_return_type(scalar.get(), type.get());
    duckdb_scalar_function_set_function(scalar.get(), function);
    duckdb_scalar_function_set_init(scalar.get(), InitEngine);
    duckdb_scalar_function_set_extra_info(
        scalar.get(), new JavaScriptLimits(limits),
        [](void* limits) { delete static_cast<JavaScriptLimits*>(limits); });
    duckdb_scalar_function_set_special_handling(scalar.get());
    duckdb_scalar_function_set_volatile(scalar.get());
    if (duckdb_register_scalar_function(connection, scalar.get()) == DuckDBError) {
      throw BackendError(std::string("DuckDB failed to register ") + name);
    }
  };
  add("bq_js_bool", DUCKDB_TYPE_BOOLEAN, JavaScriptCall<Result::kBool>);
  add("bq_js_double", DUCKDB_TYPE_DOUBLE, JavaScriptCall<Result::kDouble>);
  add("bq_js_string", DUCKDB_TYPE_VARCHAR, JavaScriptCall<Result::kString>);
  add("bq_js_int64", DUCKDB_TYPE_BIGINT, JavaScriptCall<Result::kInt64>);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
