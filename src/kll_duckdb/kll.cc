#include "src/kll_duckdb/kll.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb::kll_duckdb {
namespace {

using namespace duckdb_functions;

// This reader deliberately accepts only the observed encoding forms. In particular, an
// unfamiliar version or wire type must not be interpreted as an empty/default field.
struct InvalidSketch : std::runtime_error {
  InvalidSketch() : std::runtime_error("Invalid input sketch bytes") {}
};

uint64_t Varint(std::string_view& bytes) {
  uint64_t value = 0;
  for (unsigned shift = 0; shift < 70; shift += 7) {
    if (bytes.empty()) {
      throw InvalidSketch();
    }
    const auto byte = static_cast<unsigned char>(bytes.front());
    bytes.remove_prefix(1);
    if (shift == 63 && byte > 1) {
      throw InvalidSketch();
    }
    value |= static_cast<uint64_t>(byte & 127U) << shift;
    if (byte < 128) {
      return value;
    }
  }
  throw InvalidSketch();
}

struct Field {
  uint64_t number;
  unsigned wire;
  uint64_t integer = 0;
  std::string_view bytes = {};
};

std::vector<Field> Fields(std::string_view bytes) {
  std::vector<Field> fields;
  while (!bytes.empty()) {
    const uint64_t tag = Varint(bytes);
    if (tag >> 3U == 0 || tag >> 3U > 536870911) {
      throw InvalidSketch();
    }
    Field field{.number = tag >> 3U, .wire = static_cast<unsigned>(tag & 7U)};
    if (field.wire == 0) {
      field.integer = Varint(bytes);
    } else if (field.wire == 2) {
      const uint64_t size = Varint(bytes);
      if (size > bytes.size()) {
        throw InvalidSketch();
      }
      field.bytes = bytes.substr(0, size);
      bytes.remove_prefix(size);
    } else {
      throw InvalidSketch();
    }
    fields.push_back(field);
  }
  return fields;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): protobuf field number then wire type.
Field One(const std::vector<Field>& fields, uint64_t number, unsigned wire, bool required = true) {
  Field result{.number = 0, .wire = wire};
  for (const auto& field : fields) {
    if (field.number != number) {
      continue;
    }
    if (result.number != 0 || field.wire != wire) {
      throw InvalidSketch();
    }
    result = field;
  }
  if (required && result.number == 0) {
    throw InvalidSketch();
  }
  return result;
}

template <typename T>
std::vector<T> Values(std::string_view bytes, bool delta) {
  std::vector<T> values;
  if constexpr (std::is_same_v<T, int64_t>) {
    uint64_t sum = 0;
    while (!bytes.empty()) {
      const uint64_t value = Varint(bytes);
      sum = delta ? sum + value : value;
      values.push_back(std::bit_cast<int64_t>(sum));
    }
  } else {
    while (!bytes.empty()) {
      if (bytes.size() < 8) {
        throw InvalidSketch();
      }
      uint64_t bits = 0;
      for (unsigned i = 0; i < 8; ++i) {
        bits |= static_cast<uint64_t>(static_cast<unsigned char>(bytes.at(i))) << (i * 8);
      }
      bytes.remove_prefix(8);
      values.push_back(std::bit_cast<double>(bits));
    }
  }
  return values;
}

template <typename T>
T Single(std::string_view bytes) {
  const auto values = Values<T>(bytes, false);
  if (values.size() != 1) {
    throw InvalidSketch();
  }
  return values.front();
}

template <typename T>
struct Sketch {
  struct Item {
    T value;
    uint64_t weight;
  };
  T minimum;
  T maximum;
  std::vector<Item> items = {};
  uint64_t mass = 0;

  void Add(T value, uint64_t weight) {
    if (weight > std::numeric_limits<uint64_t>::max() - mass) {
      throw InvalidSketch();
    }
    mass += weight;
    if (weight != 0) {
      items.push_back({value, weight});
    }
  }

  // Integer rank arithmetic preserves every INT64 bit, including across the sign boundary.
  [[nodiscard]] T Quantile(uint64_t i, uint64_t k) const {
    if (i == 0) {
      return minimum;
    }
    if (i == k) {
      return maximum;
    }
    const auto rank = ((static_cast<unsigned __int128>(i) * mass) + k - 1) / k;
    const auto item =
        std::lower_bound(items.begin(), items.end(), rank,
                         [](const Item& item, auto target) { return item.weight < target; });
    if (item == items.end()) {
      throw InvalidSketch();
    }
    return item->value;
  }
};

template <typename T>
absl::StatusOr<Sketch<T>> Decode(std::string_view bytes) {
  const auto outer = Fields(bytes);
  if (One(outer, 1, 0).integer != 113 || One(outer, 2, 0).integer == 0) {
    throw InvalidSketch();
  }
  const auto version = One(outer, 3, 0, false);
  if (version.number != 0 && version.integer != 1 && version.integer != 2) {
    throw InvalidSketch();
  }
  const uint64_t type = One(outer, 4, 0).integer;
  if (type != 4 && type != 10) {
    throw InvalidSketch();
  }
  constexpr bool kInt = std::is_same_v<T, int64_t>;
  if (type != (kInt ? 4U : 10U)) {
    return absl::InvalidArgumentError(
        kInt ? "Expected sketch value type INT64 but found FLOAT64 type"
             : "Expected sketch value type FLOAT64 but found INT64 type");
  }
  const auto payload = Fields(One(outer, 113, 2).bytes);
  if (One(payload, 1, 0).integer == 0 || One(payload, 2, 0).integer == 0 ||
      One(payload, 2, 0).integer > 100000) {
    throw InvalidSketch();
  }
  Sketch<T> sketch{
      .minimum = Single<T>(One(payload, 3, 2).bytes),
      .maximum = Single<T>(One(payload, 4, 2).bytes),
  };
  unsigned level = 0;
  for (const auto& field : payload) {
    if (field.number != 5) {
      continue;
    }
    if (field.wire != 2 || level >= 64) {
      throw InvalidSketch();
    }
    const auto fields = Fields(field.bytes);
    // The other typed representation is not an empty level of the requested type.
    if (One(fields, kInt ? 1 : 2, 2, false).number != 0) {
      throw InvalidSketch();
    }
    for (T value : Values<T>(One(fields, kInt ? 2 : 1, 2).bytes, true)) {
      sketch.Add(value, uint64_t{1} << level);
    }
    ++level;
  }
  const auto sampler = One(payload, 6, 2, false);
  if (sampler.number != 0) {
    const auto fields = Fields(sampler.bytes);
    const auto weight = One(fields, 2, 0).integer;
    One(fields, 3, 0);
    const T value = Single<T>(One(fields, 1, 2).bytes);
    sketch.Add(value, weight);
  }
  if (level == 0 || sketch.mass == 0) {
    throw InvalidSketch();
  }
  std::sort(sketch.items.begin(), sketch.items.end(), [](const auto& left, const auto& right) {
    if constexpr (!kInt) {
      if (std::isnan(left.value)) {
        return !std::isnan(right.value);
      }
      if (std::isnan(right.value)) {
        return false;
      }
    }
    return left.value < right.value;
  });
  uint64_t cumulative = 0;
  for (auto& item : sketch.items) {
    cumulative += item.weight;
    item.weight = cumulative;
  }
  return sketch;
}

template <typename T>
absl::StatusOr<std::optional<T>> ExtractPoint(const Arguments& arguments) {
  const std::string name = std::string("KLL_QUANTILES.EXTRACT_POINT_") +
                           (std::is_same_v<T, int64_t> ? "INT64" : "FLOAT64");
  if (arguments.IsNull(1)) {
    return absl::InvalidArgumentError("Argument 2 to " + name + " must be non-NULL");
  }
  const double phi = arguments.Double(1);
  if (std::isnan(phi) || phi < 0 || phi > 1) {
    return absl::InvalidArgumentError("Argument 2 to " + name + " must be between 0 and 1");
  }
  if (arguments.IsNull(0)) {
    return std::optional<T>{};
  }
  const auto error = [&name](std::string_view message) {
    return absl::InvalidArgumentError(std::string(message) + "; error in " + name + " expression");
  };
  try {
    const auto sketch = Decode<T>(arguments.StringView(0));
    if (!sketch.ok()) {
      return error(sketch.status().message());
    }
    if (phi == 0) {
      return sketch->minimum;
    }
    if (phi == 1) {
      return sketch->maximum;
    }
    // Use the retained mass, including the sampler, not the envelope's input count.
    // Multiply in FLOAT64: e.g. phi=.4 with mass=5 must select rank 2, not 3.
    const auto rank =
        static_cast<unsigned __int128>(std::ceil(phi * static_cast<double>(sketch->mass)));
    const auto item =
        std::lower_bound(sketch->items.begin(), sketch->items.end(), rank,
                         [](const auto& item, auto target) { return item.weight < target; });
    if (item == sketch->items.end()) {
      throw InvalidSketch();
    }
    return item->value;
  } catch (const InvalidSketch& invalid) {
    return error(invalid.what());
  }
}

template <typename T>
absl::StatusOr<std::optional<std::vector<T>>> Extract(const Arguments& arguments) {
  const auto error = [](const std::string& message) {
    return absl::InvalidArgumentError(message + "; error in KLL_QUANTILES.EXTRACT_" +
                                      (std::is_same_v<T, int64_t> ? "INT64" : "FLOAT64") +
                                      " expression");
  };
  if (arguments.IsNull(1)) {
    return error("num_quantiles must be non-NULL");
  }
  const int64_t count = arguments.Int(1);
  if (count < 1 || count > 100000) {
    return error("Provided num_quantiles: " + std::to_string(count) +
                 " but num_quantiles must be in the range [1, 100000]");
  }
  if (arguments.IsNull(0)) {
    return std::optional<std::vector<T>>{};
  }
  try {
    const auto sketch = Decode<T>(arguments.StringView(0));
    if (!sketch.ok()) {
      return error(std::string(sketch.status().message()));
    }
    std::vector<T> result;
    result.reserve(count + 1);
    for (int64_t i = 0; i <= count; ++i) {
      result.push_back(sketch->Quantile(i, count));
    }
    return std::optional<std::vector<T>>(std::move(result));
  } catch (const InvalidSketch& invalid) {
    return error(invalid.what());
  }
}

}  // namespace

void RegisterKllFunctions(duckdb_connection connection) {
  Register(connection, "bq_kll_extract_point_int64", {kBlob, kDouble}, kBigint,
           EachRow<ExtractPoint<int64_t>, false>, /*nulls=*/false);
  Register(connection, "bq_kll_extract_point_float64", {kBlob, kDouble}, kDouble,
           EachRow<ExtractPoint<double>, false>, /*nulls=*/false);
  LogicalType const bigint(duckdb_create_logical_type(kBigint));
  LogicalType const real(duckdb_create_logical_type(kDouble));
  LogicalType const ints(duckdb_create_list_type(bigint.get()));
  LogicalType const doubles(duckdb_create_list_type(real.get()));
  Register(connection, "bq_kll_extract_int64", {kBlob, kBigint}, ints.get(),
           EachRow<Extract<int64_t>, false>, /*nulls=*/false);
  Register(connection, "bq_kll_extract_float64", {kBlob, kBigint}, doubles.get(),
           EachRow<Extract<double>, false>, /*nulls=*/false);
}

}  // namespace bigquery_emulator_duckdb::kll_duckdb
