#include "src/udf_duckdb/python.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "duckdb.h"
#include "nlohmann/json.hpp"
#include "src/backend_error.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/udf/python/options.h"
#include "src/udf/python/worker.h"

namespace bigquery_emulator_duckdb::udf_duckdb {
using namespace duckdb_functions;
namespace {
using nlohmann::json;
using udf::PythonOptions;
using udf::python::Worker;

json Argument(const Arguments& arguments, idx_t column) {
  if (arguments.IsNull(column)) {
    return nullptr;
  }
  const LogicalType type(duckdb_vector_get_column_type(arguments.Vector(column)));
  switch (duckdb_get_type_id(type.get())) {
    case DUCKDB_TYPE_BOOLEAN:
      return json::array({"BOOL", arguments.Bool(column)});
    case DUCKDB_TYPE_BIGINT:
      return json::array({"INT64", arguments.Int(column)});
    case DUCKDB_TYPE_DOUBLE: {
      const double value = arguments.Double(column);
      const std::string encoded = std::isfinite(value) ? json(value).dump()
                                  : std::isnan(value)  ? "nan"
                                  : value < 0          ? "-inf"
                                                       : "inf";
      return json::array({"FLOAT64", encoded});
    }
    case DUCKDB_TYPE_VARCHAR:
      return json::array({"STRING", arguments.String(column)});
    case DUCKDB_TYPE_BLOB:
      return json::array({"BYTES", absl::BytesToHexString(arguments.String(column))});
    default:
      throw std::runtime_error("Unsupported Python UDF argument type");
  }
}

template <typename T, bool kBytes = false>
void Call(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  try {
    auto* worker = static_cast<Worker*>(duckdb_scalar_function_get_state(info));
    if (worker == nullptr) {
      throw std::runtime_error("Python UDF called without a worker");
    }
    EachRow(
        info, input, output,
        [&](const Arguments& arguments) -> absl::StatusOr<std::optional<T>> {
          json cells = json::array();
          for (idx_t column = 1; column < duckdb_data_chunk_get_column_count(input); ++column) {
            cells.push_back(Argument(arguments, column));
          }
          const json value = worker->Call(arguments.String(0), std::move(cells));
          if (value.is_null()) {
            return std::nullopt;
          }
          if constexpr (std::is_same_v<T, double>) {
            const std::string text = value.get<std::string>();
            double result = 0;
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), result);
            if (error != std::errc() || end != text.data() + text.size()) {
              return absl::InvalidArgumentError("Invalid Python FLOAT64 result");
            }
            return result;
          } else if constexpr (kBytes) {
            std::string decoded;
            if (!absl::HexStringToBytes(value.get<std::string>(), &decoded)) {
              return absl::InvalidArgumentError("Invalid Python BYTES result");
            }
            return decoded;
          } else {
            return value.get<T>();
          }
        },
        /*nulls=*/false);
  } catch (const std::exception& error) {
    duckdb_scalar_function_set_error(info, error.what());
  }
}

void InitWorker(duckdb_init_info info) {
  try {
    const auto* options =
        static_cast<const PythonOptions*>(duckdb_scalar_function_init_get_extra_info(info));
    duckdb_scalar_function_init_set_state(info, new Worker(*options),
                                          [](void* state) { delete static_cast<Worker*>(state); });
  } catch (const std::exception& error) {
    duckdb_scalar_function_init_set_error(info, error.what());
  }
}
}  // namespace

void RegisterPythonFunctions(duckdb_connection connection, const udf::PythonOptions& options) {
  const auto add = [&](const char* name, duckdb_type result, duckdb_scalar_function_t callback) {
    Handle<duckdb_scalar_function, duckdb_destroy_scalar_function> const scalar(
        duckdb_create_scalar_function());
    duckdb_scalar_function_set_name(scalar.get(), name);
    const LogicalType source(duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
    duckdb_scalar_function_add_parameter(scalar.get(), source.get());
    const LogicalType any(duckdb_create_logical_type(DUCKDB_TYPE_ANY));
    duckdb_scalar_function_set_varargs(scalar.get(), any.get());
    const LogicalType type(duckdb_create_logical_type(result));
    duckdb_scalar_function_set_return_type(scalar.get(), type.get());
    duckdb_scalar_function_set_function(scalar.get(), callback);
    duckdb_scalar_function_set_init(scalar.get(), InitWorker);
    duckdb_scalar_function_set_extra_info(
        scalar.get(), new udf::PythonOptions(options),
        [](void* data) { delete static_cast<udf::PythonOptions*>(data); });
    duckdb_scalar_function_set_special_handling(scalar.get());
    duckdb_scalar_function_set_volatile(scalar.get());
    if (duckdb_register_scalar_function(connection, scalar.get()) == DuckDBError) {
      throw BackendError(std::string("DuckDB failed to register ") + name);
    }
  };
  add("bq_py_bool", DUCKDB_TYPE_BOOLEAN, Call<bool>);
  add("bq_py_int64", DUCKDB_TYPE_BIGINT, Call<int64_t>);
  add("bq_py_double", DUCKDB_TYPE_DOUBLE, Call<double>);
  add("bq_py_string", DUCKDB_TYPE_VARCHAR, Call<std::string>);
  add("bq_py_bytes", DUCKDB_TYPE_BLOB, Call<std::string, true>);
}
}  // namespace bigquery_emulator_duckdb::udf_duckdb
