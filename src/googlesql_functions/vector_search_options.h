#pragma once

#include <string_view>

#include "absl/status/status.h"

namespace googlesql_functions {

// Validates VECTOR_SEARCH's options with BigQuery's errors, observed in
// tests/e2e/goclient/testdata/scalars/vector_search.txt. They select how a vector index is
// searched; the emulator has no vector indexes, so every search is the exact brute-force search
// that BigQuery falls back to without one.
absl::Status CheckVectorSearchOptions(std::string_view text);

}  // namespace googlesql_functions
