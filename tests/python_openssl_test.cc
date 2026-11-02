// The embedded CPython's SSL and hash modules are compiled against python-build-standalone's
// OpenSSL headers but link Bazel's OpenSSL, which a dependency upgrade can move to another ABI.
// OpenSSL keeps its ABI within a major version, and a module needs at least the minor version
// whose headers it was compiled against.
#include <Python.h>  // IWYU pragma: keep

#include <filesystem>
#include <memory>
#include <string>

#include "gtest/gtest.h"
#include "python_runtime_path.h"
#include "rules_cc/cc/runfiles/runfiles.h"

namespace {

TEST(PythonOpenSslTest, LinkedOpenSslIsCompatibleWithCompiledHeaders) {
  using rules_cc::cc::runfiles::Runfiles;
  std::unique_ptr<Runfiles> const files(Runfiles::CreateForTest());
  ASSERT_NE(files, nullptr);
  const auto python = std::filesystem::path(files->Rlocation(kPythonRuntime));
  const std::string home = python.parent_path().parent_path().string();
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  config.site_import = 0;
  PyStatus status = PyConfig_SetBytesString(&config, &config.home, home.c_str());
  if (PyStatus_Exception(status) == 0) {
    status = Py_InitializeFromConfig(&config);
  }
  PyConfig_Clear(&config);
  ASSERT_EQ(PyStatus_Exception(status), 0);

  PyObject* globals = PyDict_New();
  PyObject* result = PyRun_String(
      "(lambda compiled, linked: (compiled[0] == linked[0] and compiled[:2] <= linked[:2], "
      "f'compiled against OpenSSL {compiled[:3]}, linked {linked[:3]}'))("
      "__import__('ssl')._OPENSSL_API_VERSION, __import__('ssl').OPENSSL_VERSION_INFO)",
      Py_eval_input, globals, globals);
  if (result == nullptr) {
    PyErr_Print();
  }
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyTuple_GetItem(result, 0), Py_True) << PyUnicode_AsUTF8(PyTuple_GetItem(result, 1));
  Py_DECREF(result);
  Py_DECREF(globals);
  EXPECT_EQ(Py_FinalizeEx(), 0);
}

}  // namespace
