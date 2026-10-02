#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace bigquery_emulator_duckdb {

// Files created in the system temporary directory, which honors TMPDIR, and removed when this
// object is destroyed.
class TemporaryFiles {
 public:
  TemporaryFiles() = default;
  ~TemporaryFiles();

  TemporaryFiles(const TemporaryFiles&) = delete;
  TemporaryFiles& operator=(const TemporaryFiles&) = delete;

  // Creates an empty file whose name ends in `suffix` and returns its path.
  std::string Create(std::string_view suffix = "");
  // Creates a file holding `contents` and returns its path.
  std::string Write(std::string_view contents);

 private:
  std::vector<std::string> paths_;
};

}  // namespace bigquery_emulator_duckdb
