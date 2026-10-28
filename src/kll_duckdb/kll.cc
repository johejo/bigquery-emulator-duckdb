#include "src/kll_duckdb/kll.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "src/backend_error.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb::kll_duckdb {
namespace {

using namespace duckdb_functions;

// This reader deliberately accepts only the observed encoding forms. In particular, an
// unfamiliar version or wire type must not be interpreted as an empty/default field.
struct InvalidSketch : std::runtime_error {
  explicit InvalidSketch(const std::string& message = "Invalid input sketch bytes")
      : std::runtime_error(message) {}
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
  uint64_t count = 0;
  uint64_t precision = 1000;

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
      .count = One(outer, 2, 0).integer,
      .precision = One(payload, 2, 0).integer,
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

// Unbiased randomized pair compactors, with a conservative fixed capacity at
// every level. Compared with geometrically shrinking KLL capacities this uses
// more space: O(precision * log(n)). Squared rank error is O(n^2 / capacity^2).
// Weighted insertion decomposes weights into bits instead of expanding rows.
//
// Deferred: at the default precision a level holds up to 64000 values, so each
// group's state and serialized sketch can be hundreds of KB per level, far more
// than BigQuery's (its precision-1000 specimen kept 3339 values in a level), and
// many groups multiply that. Shrinking capacities toward the top level, as KLL
// does, would cut this but needs its own rank-error argument for each precision;
// see docs/compatibility.md#approximate-aggregates.
template <typename T>
bool Less(T a, T b) {
  if constexpr (std::is_same_v<T, double>) {
    if (std::isnan(a)) {
      return !std::isnan(b);
    }
    if (std::isnan(b)) {
      return false;
    }
  }
  return a < b;
}

void PutVarint(std::string& out, uint64_t value) {
  while (value >= 128) {
    out.push_back(static_cast<char>((value & 127U) | 128U));
    value >>= 7U;
  }
  out.push_back(static_cast<char>(value));
}
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): protobuf field number then value.
void PutInteger(std::string& out, uint64_t field, uint64_t value) {
  PutVarint(out, field << 3U);
  PutVarint(out, value);
}
void PutBytes(std::string& out, uint64_t field, std::string_view value) {
  PutVarint(out, (field << 3U) | 2U);
  PutVarint(out, value.size());
  out.append(value);
}
template <typename T>
std::string EncodeValue(T value) {
  std::string out;
  const auto bits = std::bit_cast<uint64_t>(value);
  if constexpr (std::is_same_v<T, int64_t>) {
    PutVarint(out, bits);
  } else {
    for (unsigned i = 0; i < 8; ++i) {
      out.push_back(static_cast<char>(bits >> (8 * i)));
    }
  }
  return out;
}

template <typename T>
struct Builder {
  uint64_t precision = 1000;
  uint64_t count = 0;
  uint64_t mass = 0;
  T minimum = {};
  T maximum = {};
  std::vector<std::vector<T>> levels = {};

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): minimum then maximum.
  void Extrema(T low, T high) {
    if (mass == 0 || Less(low, minimum)) {
      minimum = low;
    }
    if (mass == 0 || Less(maximum, high)) {
      maximum = high;
    }
  }
  // Sorts a level only when it overflows, which then leaves at most one value, so
  // each insert costs O(log precision) amortized.
  void Compact() {
    for (std::size_t level = 0; level < levels.size(); ++level) {
      if (levels.at(level).size() <= 64 * precision) {
        continue;
      }
      if (level == 63) {
        throw InvalidSketch();
      }
      if (level + 1 == levels.size()) {
        levels.emplace_back();
      }
      auto& values = levels.at(level);
      std::sort(values.begin(), values.end(), Less<T>);
      static thread_local std::mt19937_64 random(std::random_device{}());
      const std::size_t parity = random() & 1U;
      const std::size_t end = values.size() - (values.size() % 2);
      for (std::size_t i = parity; i < end; i += 2) {
        levels.at(level + 1).push_back(values.at(i));
      }
      if (end != values.size()) {
        values.front() = values.back();
        values.resize(1);
      } else {
        values.clear();
      }
    }
  }
  void Retain(T value, uint64_t weight) {
    for (unsigned level = 0; weight != 0; ++level, weight >>= 1U) {
      if ((weight & 1U) == 0) {
        continue;
      }
      if (levels.size() <= level) {
        levels.resize(level + 1);
      }
      levels.at(level).push_back(value);
    }
  }
  void Add(T value, uint64_t weight) {
    if (weight > std::numeric_limits<uint64_t>::max() - mass ||
        weight > std::numeric_limits<uint64_t>::max() - count) {
      throw InvalidSketch();
    }
    if constexpr (std::is_same_v<T, double>) {
      if (value == 0 && std::signbit(value)) {
        value = 0;  // BigQuery canonicalizes signed zero.
      }
    }
    Extrema(value, value);
    mass += weight;
    count += weight;
    Retain(value, weight);
    Compact();
  }
  void Merge(const Builder& from) {
    if (from.mass == 0) {
      return;
    }
    if (from.mass > std::numeric_limits<uint64_t>::max() - mass ||
        from.count > std::numeric_limits<uint64_t>::max() - count) {
      throw InvalidSketch();
    }
    Extrema(from.minimum, from.maximum);
    // An exact input keeps the mergee's precision, as BigQuery does in either order
    // (tools/kll/merge_precision.answers.txt), even when the mergee is compacted; a
    // compacted input lowers it, as documented.
    if (mass == 0) {
      precision = from.precision;
    } else if (from.levels.size() > 1) {
      precision = std::min(precision, from.precision);
    }
    mass += from.mass;
    count += from.count;
    levels.resize(std::max(levels.size(), from.levels.size()));
    for (std::size_t i = 0; i < from.levels.size(); ++i) {
      levels.at(i).insert(levels.at(i).end(), from.levels.at(i).begin(), from.levels.at(i).end());
    }
    Compact();
  }
  absl::Status Read(std::string_view bytes) {
    const auto sketch = Decode<T>(bytes);
    if (!sketch.ok()) {
      return sketch.status();
    }
    Builder from;
    from.minimum = sketch->minimum;
    from.maximum = sketch->maximum;
    from.precision = sketch->precision;
    from.mass = sketch->mass;
    from.count = sketch->count;
    uint64_t previous = 0;
    for (const auto& item : sketch->items) {
      from.Retain(item.value, item.weight - previous);
      previous = item.weight;
    }
    Merge(from);
    return absl::OkStatus();
  }
  [[nodiscard]] std::string Serialize() const {
    std::string payload;
    PutInteger(payload, 1, 64 * precision);
    PutInteger(payload, 2, precision);
    PutBytes(payload, 3, EncodeValue(minimum));
    PutBytes(payload, 4, EncodeValue(maximum));
    for (auto values : levels) {
      std::sort(values.begin(), values.end(), Less<T>);
      std::string encoded;
      // NOLINTNEXTLINE(misc-const-correctness): updated by the INT64 instantiation.
      uint64_t previous = 0;
      for (T value : values) {
        if constexpr (std::is_same_v<T, int64_t>) {
          const auto bits = std::bit_cast<uint64_t>(value);
          PutVarint(encoded, bits - previous);
          previous = bits;
        } else {
          encoded += EncodeValue(value);
        }
      }
      std::string level;
      PutBytes(level, std::is_same_v<T, int64_t> ? 2 : 1, encoded);
      PutBytes(payload, 5, level);
    }
    std::string out;
    PutInteger(out, 1, 113);
    PutInteger(out, 2, count);
    PutInteger(out, 4, std::is_same_v<T, int64_t> ? 4 : 10);
    PutBytes(out, 113, payload);
    return out;
  }
};

using AggregateState = std::variant<std::monostate, Builder<int64_t>, Builder<double>>;
AggregateState*& State(duckdb_aggregate_state state) {
  return *reinterpret_cast<AggregateState**>(state);
}
idx_t StateSize(duckdb_function_info /*info*/) { return sizeof(AggregateState*); }
void InitState(duckdb_function_info /*info*/, duckdb_aggregate_state state) {
  State(state) = new AggregateState();
}
void DestroyStates(duckdb_aggregate_state* states, idx_t count) {
  for (idx_t i = 0; i < count; ++i) {
    delete State(states[i]);
  }
}

template <typename T>
void AddValues(duckdb_function_info info, duckdb_data_chunk input, duckdb_aggregate_state* states) {
  const std::string name =
      std::string("KLL_QUANTILES.INIT_") + (std::is_same_v<T, int64_t> ? "INT64" : "FLOAT64");
  try {
    for (idx_t row = 0; row < duckdb_data_chunk_get_size(input); ++row) {
      const Arguments args(input, row);
      // The finish expression validates precision even for an empty group.
      if (args.IsNull(1) || args.Int(1) < 1 || args.Int(1) > 100000) {
        continue;
      }
      if (args.IsNull(0) || args.IsNull(2)) {
        continue;
      }
      const int64_t weight = args.Int(2);
      if (weight < 1 || weight > 2147483647) {
        duckdb_aggregate_function_set_error(
            info, ("Provided Weight: " + std::to_string(weight) +
                   " but Weight needs to be in the inclusive interval [1, 2147483647]; error in " +
                   name + " function.")
                      .c_str());
        return;
      }
      auto& state = *State(states[row]);
      if (std::holds_alternative<std::monostate>(state)) {
        state = Builder<T>{};
        std::get<Builder<T>>(state).precision = args.Int(1);
      }
      T value;
      if constexpr (std::is_same_v<T, int64_t>) {
        value = args.Int(0);
      } else {
        value = args.Double(0);
      }
      std::get<Builder<T>>(state).Add(value, weight);
    }
  } catch (const InvalidSketch& error) {
    duckdb_aggregate_function_set_error(info, error.what());
  }
}

void MergeState(AggregateState& target, const AggregateState& source) {
  std::visit(
      [&](const auto& from) {
        using B = std::decay_t<decltype(from)>;
        if constexpr (!std::is_same_v<B, std::monostate>) {
          if (std::holds_alternative<std::monostate>(target)) {
            target = from;
          } else if (auto* to = std::get_if<B>(&target)) {
            to->Merge(from);
          } else {
            const bool integer = std::holds_alternative<Builder<int64_t>>(target);
            throw InvalidSketch(
                std::string("Merging sketches with different value types is not allowed. ") +
                (integer ? "Expected sketch value type INT64 but found FLOAT64 type"
                         : "Expected sketch value type FLOAT64 but found INT64 type") +
                "; error in KLL_QUANTILES.MERGE_PARTIAL function.");
          }
        }
      },
      source);
}
template <uint64_t kType = 0>
void MergeSketches(duckdb_function_info info, duckdb_data_chunk input,
                   duckdb_aggregate_state* states) {
  try {
    for (idx_t row = 0; row < duckdb_data_chunk_get_size(input); ++row) {
      const Arguments args(input, row);
      if (args.IsNull(0)) {
        continue;
      }
      const auto bytes = args.StringView(0);
      const auto type = One(Fields(bytes), 4, 0).integer;
      if constexpr (kType != 0) {
        if (type != kType && (type == 4 || type == 10)) {
          duckdb_aggregate_function_set_error(
              info, kType == 4 ? "Expected sketch value type INT64 but found FLOAT64 type"
                               : "Expected sketch value type FLOAT64 but found INT64 type");
          return;
        }
      }
      auto& state = *State(states[row]);
      if (std::holds_alternative<std::monostate>(state)) {
        if (type == 4) {
          state = Builder<int64_t>{};
        } else if (type == 10) {
          state = Builder<double>{};
        } else {
          throw InvalidSketch();
        }
      }
      const auto status = std::visit(
          [&](auto& builder) -> absl::Status {
            using B = std::decay_t<decltype(builder)>;
            if constexpr (std::is_same_v<B, std::monostate>) {
              return absl::InternalError("Missing KLL state");
            } else {
              return builder.Read(bytes);
            }
          },
          state);
      if (!status.ok()) {
        duckdb_aggregate_function_set_error(
            info,
            ("Merging sketches with different value types is not allowed. " +
             std::string(status.message()) + "; error in KLL_QUANTILES.MERGE_PARTIAL function.")
                .c_str());
        return;
      }
    }
  } catch (const InvalidSketch& error) {
    duckdb_aggregate_function_set_error(info, error.what());
  }
}
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): DuckDB's callback.
void CombineStates(duckdb_function_info info, duckdb_aggregate_state* source,
                   duckdb_aggregate_state* target, idx_t count) {
  try {
    for (idx_t i = 0; i < count; ++i) {
      MergeState(*State(target[i]), *State(source[i]));
    }
  } catch (const InvalidSketch& error) {
    duckdb_aggregate_function_set_error(info, error.what());
  }
}
// NOLINTBEGIN(bugprone-easily-swappable-parameters): DuckDB's callback.
void FinalizeStates(duckdb_function_info /*info*/, duckdb_aggregate_state* states,
                    duckdb_vector result, idx_t count, idx_t offset) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  duckdb_vector_ensure_validity_writable(result);
  for (idx_t i = 0; i < count; ++i) {
    std::visit(
        [&](const auto& builder) {
          using B = std::decay_t<decltype(builder)>;
          if constexpr (std::is_same_v<B, std::monostate>) {
            SetResult(result, offset + i, std::optional<std::string>{});
          } else {
            SetResult(result, offset + i, builder.Serialize());
          }
        },
        *State(states[i]));
  }
}
void RegisterAggregate(duckdb_connection connection, const char* name,
                       const std::vector<duckdb_type>& parameters,
                       duckdb_aggregate_update_t update) {
  const Handle<duckdb_aggregate_function, duckdb_destroy_aggregate_function> function(
      duckdb_create_aggregate_function());
  duckdb_aggregate_function_set_name(function.get(), name);
  for (const auto parameter : parameters) {
    LogicalType const type(duckdb_create_logical_type(parameter));
    duckdb_aggregate_function_add_parameter(function.get(), type.get());
  }
  LogicalType const blob(duckdb_create_logical_type(kBlob));
  duckdb_aggregate_function_set_return_type(function.get(), blob.get());
  duckdb_aggregate_function_set_functions(function.get(), StateSize, InitState, update,
                                          CombineStates, FinalizeStates);
  duckdb_aggregate_function_set_destructor(function.get(), DestroyStates);
  duckdb_aggregate_function_set_special_handling(function.get());
  if (duckdb_register_aggregate_function(connection, function.get()) == DuckDBError) {
    throw BackendError(std::string("DuckDB failed to register ") + name);
  }
}

}  // namespace

void RegisterKllFunctions(duckdb_connection connection) {
  RegisterAggregate(connection, "bq_kll_init_int64", {kBigint, kBigint, kBigint},
                    AddValues<int64_t>);
  RegisterAggregate(connection, "bq_kll_init_float64", {kDouble, kBigint, kBigint},
                    AddValues<double>);
  RegisterAggregate(connection, "bq_kll_merge_partial", {kBlob}, MergeSketches<>);
  RegisterAggregate(connection, "bq_kll_merge_int64", {kBlob}, MergeSketches<4>);
  RegisterAggregate(connection, "bq_kll_merge_float64", {kBlob}, MergeSketches<10>);
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
