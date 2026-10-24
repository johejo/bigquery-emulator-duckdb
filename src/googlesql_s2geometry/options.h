#pragma once

namespace googlesql_s2geometry {

// Execution-independent choices; the embedding application selects its compatibility policy.
struct Options {
  // Expand finite bounds outward by one representable degree, keeping poles and full
  // longitude intervals in range. BigQuery does this even for zero (into subnormal values).
  bool expand_bounding_box = false;
};

}  // namespace googlesql_s2geometry
