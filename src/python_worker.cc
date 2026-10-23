// Only this executable links libpython. The emulator never initializes an interpreter.
#include <Python.h>  // IWYU pragma: keep
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "absl/strings/escaping.h"
#include "nlohmann/json.hpp"
#include "python_runtime_path.h"
#include "rules_cc/cc/runfiles/runfiles.h"

namespace {
using nlohmann::json;
struct DeleteObject {
  void operator()(PyObject* object) const { Py_XDECREF(object); }
};
using Object = std::unique_ptr<PyObject, DeleteObject>;

std::runtime_error PythonError() {
  PyObject* type = nullptr;
  PyObject* value = nullptr;
  PyObject* traceback = nullptr;
  PyErr_Fetch(&type, &value, &traceback);
  PyErr_NormalizeException(&type, &value, &traceback);
  Object const owned_type(type);
  Object const owned_value(value);
  Object const owned_traceback(traceback);
  Object const name(type == nullptr ? nullptr : PyObject_GetAttrString(type, "__name__"));
  Object const text(value == nullptr ? nullptr : PyObject_Str(value));
  const char* name_text = name ? PyUnicode_AsUTF8(name.get()) : nullptr;
  const char* value_text = text ? PyUnicode_AsUTF8(text.get()) : nullptr;
  std::string message = "Python UDF: ";
  message += name_text == nullptr ? "error" : name_text;
  if (value_text != nullptr) {
    message += ": " + std::string(value_text);
  }
  PyErr_Clear();
  return std::runtime_error(message);
}

Object Checked(PyObject* value) {
  if (value == nullptr) {
    throw PythonError();
  }
  return Object(value);
}

Object Argument(const json& cell) {
  if (cell.is_null()) {
    return Checked(Py_NewRef(Py_None));
  }
  const std::string type = cell.at(0);
  const auto& value = cell.at(1);
  if (type == "BOOL") {
    return Checked(PyBool_FromLong(value.get<bool>() ? 1 : 0));
  }
  if (type == "INT64") {
    return Checked(PyLong_FromLongLong(value.get<int64_t>()));
  }
  if (type == "FLOAT64") {
    Object const text = Checked(PyUnicode_FromString(value.get<std::string>().c_str()));
    return Checked(PyFloat_FromString(text.get()));
  }
  std::string bytes = value.get<std::string>();
  if (type == "BYTES") {
    std::string decoded;
    if (!absl::HexStringToBytes(bytes, &decoded)) {
      throw std::runtime_error("Invalid BYTES argument");
    }
    return Checked(
        PyBytes_FromStringAndSize(decoded.data(), static_cast<Py_ssize_t>(decoded.size())));
  }
  return Checked(
      PyUnicode_DecodeUTF8(bytes.data(), static_cast<Py_ssize_t>(bytes.size()), "strict"));
}

json Result(PyObject* value, const std::string& type) {
  if (value == Py_None) {
    return nullptr;
  }
  if (type == "BOOL" && PyBool_Check(value)) {
    return value == Py_True;
  }
  if (type == "INT64" && PyBool_Check(value)) {
    throw std::runtime_error("Expected integer, got bool");
  }
  if (type == "INT64" && PyLong_Check(value)) {
    const int64_t number = PyLong_AsLongLong(value);
    if (PyErr_ExceptionMatches(PyExc_OverflowError) != 0) {
      PyErr_Clear();
      throw std::runtime_error("Python int too large to convert to C long");
    }
    if (PyErr_Occurred() != nullptr) {
      throw PythonError();
    }
    return number;
  }
  if (type == "FLOAT64" &&
      (PyFloat_Check(value) || (PyLong_Check(value) && !PyBool_Check(value)))) {
    const double number = PyFloat_AsDouble(value);
    if (PyErr_Occurred() != nullptr) {
      throw PythonError();
    }
    Object const converted = Checked(PyFloat_FromDouble(number));
    Object const text = Checked(PyObject_Repr(converted.get()));
    const char* utf8 = PyUnicode_AsUTF8(text.get());
    if (utf8 == nullptr) {
      throw PythonError();
    }
    return utf8;
  }
  if (type == "STRING" && PyUnicode_Check(value)) {
    Py_ssize_t size = 0;
    const char* text = PyUnicode_AsUTF8AndSize(value, &size);
    if (text == nullptr) {
      throw PythonError();
    }
    return std::string(text, static_cast<size_t>(size));
  }
  if (type == "BYTES" && PyBytes_Check(value)) {
    return absl::BytesToHexString(
        std::string_view(PyBytes_AS_STRING(value), static_cast<size_t>(PyBytes_GET_SIZE(value))));
  }
  throw std::runtime_error("Python UDF returned a value that is not a " + type);
}

struct Function {
  Object callable;
  std::string result_type;
};

class Interpreter {
 public:
  json Call(const json& request) {
    const std::string key = request.at("definition");
    auto found = functions_.find(key);
    if (found == functions_.end()) {
      const json definition = json::parse(key);
      Object const globals = Checked(PyDict_New());
      if (PyDict_SetItemString(globals.get(), "__builtins__", PyEval_GetBuiltins()) < 0) {
        throw PythonError();
      }
      Object const name = Checked(PyUnicode_FromString("__bigquery_udf__"));
      if (PyDict_SetItemString(globals.get(), "__name__", name.get()) < 0) {
        throw PythonError();
      }
      const std::string body = definition.at("body");
      // Py_CompileString consumes a C string; reject embedded NUL instead of truncating the body.
      if (body.find('\0') != std::string::npos) {
        throw std::runtime_error("Python UDF source contains NUL");
      }
      Object const code = Checked(Py_CompileString(body.c_str(), "<bigquery-udf>", Py_file_input));
      Checked(PyEval_EvalCode(code.get(), globals.get(), globals.get()));
      const std::string entry = definition.at("entry_point");
      PyObject* callable = PyDict_GetItemString(globals.get(), entry.c_str());
      if (callable == nullptr || PyCallable_Check(callable) == 0) {
        throw std::runtime_error("Python UDF entry_point is not a callable: " + entry);
      }
      found = functions_
                  .emplace(key,
                           Function{
                               .callable = Checked(Py_NewRef(callable)),
                               .result_type = definition.at("result_type").get<std::string>(),
                           })
                  .first;
    }
    const auto& arguments = request.at("arguments");
    Object const values = Checked(PyTuple_New(static_cast<Py_ssize_t>(arguments.size())));
    for (size_t i = 0; i < arguments.size(); ++i) {
      Object value = Argument(arguments.at(i));
      PyTuple_SET_ITEM(values.get(), static_cast<Py_ssize_t>(i), value.release());
    }
    Object const result = Checked(PyObject_CallObject(found->second.callable.get(), values.get()));
    return Result(result.get(), found->second.result_type);
  }

 private:
  std::map<std::string, Function> functions_;
};

void Initialize(const char* executable) {
  using rules_cc::cc::runfiles::Runfiles;
  std::unique_ptr<Runfiles> const files(Runfiles::Create(executable));
  if (!files) {
    throw std::runtime_error("Cannot locate Python runtime runfiles");
  }
  const auto python = std::filesystem::path(files->Rlocation(kPythonRuntime));
  if (!std::filesystem::exists(python)) {
    throw std::runtime_error("Cannot locate bundled Python 3.11 runtime");
  }
  // Keep the runfiles path: canonicalizing bin/python3 would lose the adjacent stdlib layout.
  const std::string home = python.parent_path().parent_path().string();
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  config.install_signal_handlers = 0;
  config.site_import = 0;
  config.write_bytecode = 0;
  PyStatus status = PyConfig_SetBytesString(&config, &config.home, home.c_str());
  if (PyStatus_Exception(status) == 0) {
    status = Py_InitializeFromConfig(&config);
  }
  const std::string error =
      PyStatus_Exception(status) != 0
          ? (status.err_msg == nullptr ? "Python initialization failed" : status.err_msg)
          : "";
  PyConfig_Clear(&config);
  if (!error.empty()) {
    throw std::runtime_error(error);
  }
}

// fd 3 is a private protocol socket; stdin/stdout/stderr are /dev/null, so print(), native
// writes and input() in a UDF cannot consume or corrupt the request stream.
void Reply(const json& value) {
  const std::string encoded = value.dump() + "\n";
  size_t offset = 0;
  while (offset < encoded.size()) {
    const ssize_t count = write(3, encoded.data() + offset, encoded.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      throw std::runtime_error("Python worker socket closed");
    }
    offset += static_cast<size_t>(count);
  }
}

int Run(int argc, char** argv) {
  if (argc != 2) {
    return 2;
  }
  const rlim_t memory = std::stoull(argv[1]);
  const rlimit limit{.rlim_cur = memory, .rlim_max = memory};
  if (setrlimit(RLIMIT_AS, &limit) != 0) {
    throw std::runtime_error("Cannot set Python worker memory limit");
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX fcntl takes an integer flag.
  if (fcntl(3, F_SETFD, FD_CLOEXEC) < 0) {
    return 1;
  }
  Initialize(argv[0]);
  {
    Interpreter interpreter;
    std::string pending;
    char buffer[8192];
    while (true) {
      const ssize_t count = read(3, buffer, sizeof(buffer));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        break;
      }
      pending.append(buffer, static_cast<size_t>(count));
      size_t end = 0;
      while ((end = pending.find('\n')) != std::string::npos) {
        const std::string line = pending.substr(0, end);
        pending.erase(0, end + 1);
        try {
          Reply({{"value", interpreter.Call(json::parse(line))}});
        } catch (const std::exception& error) {
          Reply({{"error", error.what()}});
        }
      }
    }
  }
  return Py_FinalizeEx() < 0 ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    return Run(argc, argv);
  } catch (const std::exception&) {
    return 1;
  }
}
