#include "src/temporary_files.h"

// include-cleaner requires stdlib.h for the POSIX mkstemps API; the deprecated-header checks
// suggest cstdlib instead, so suppress them on this include.
#include <stdlib.h>  // NOLINT(hicpp-deprecated-headers,modernize-deprecated-headers)
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>

#include "src/api_error.h"

namespace bigquery_emulator_duckdb {

TemporaryFiles::~TemporaryFiles() {
  for (const std::string& path : paths_) {
    unlink(path.c_str());
  }
}

std::string TemporaryFiles::Create(std::string_view suffix) {
  std::string pattern =
      (std::filesystem::temp_directory_path() / "bigquery-emulator-XXXXXX").string();
  pattern += suffix;
  const int fd = mkstemps(pattern.data(), static_cast<int>(suffix.size()));
  if (fd < 0) throw ApiError::Internal("Could not create temporary file");
  close(fd);
  paths_.push_back(pattern);
  return pattern;
}

std::string TemporaryFiles::Write(std::string_view contents) {
  std::string path = Create();
  std::ofstream stream(path, std::ios::binary);
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  stream.close();
  if (!stream) throw ApiError::Internal("Could not write temporary file");
  return path;
}

}  // namespace bigquery_emulator_duckdb
