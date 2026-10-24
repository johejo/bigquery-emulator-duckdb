#pragma once

#include <sys/types.h>  // IWYU pragma: keep -- POSIX provider of pid_t.

#include <chrono>
#include <cstdint>
#include <string>

#include "nlohmann/json_fwd.hpp"
#include "src/udf/python/options.h"

namespace bigquery_emulator_duckdb::udf::python {

// One thread's lazily started Python worker; destruction reaps its process group.
class Worker {
 public:
  explicit Worker(PythonOptions options);
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;
  ~Worker();

  nlohmann::json Call(const std::string& definition, nlohmann::json arguments);

 private:
  using Clock = std::chrono::steady_clock;
  void Wait(int16_t events, Clock::time_point deadline) const;
  void Start();

  PythonOptions options_;
  int socket_ = -1;
  // sys/types.h is the public POSIX provider, rather than libc's internal pid_t header.
  pid_t pid_ = -1;  // NOLINT(misc-include-cleaner)
};

}  // namespace bigquery_emulator_duckdb::udf::python
