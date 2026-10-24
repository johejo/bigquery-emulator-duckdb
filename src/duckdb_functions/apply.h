#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "duckdb.h"
#include "googlesql/public/interval_value.h"
#include "googlesql/public/numeric_value.h"
#include "src/bignumeric.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/interval.h"

// Apply<kFunction> runs a GoogleSQL function as a DuckDB scalar function, row by row. It reads
// the function's arguments from the columns in order, by the C++ type of each parameter, and
// writes its result by the result's C++ type; see Codec. It accepts the conventions of
// GoogleSQL's functions:
//
//   bool f(arguments..., Out* out, absl::Status* error)
//   absl::Status f(arguments..., Out* out)
//   absl::Status f(arguments..., Out* out, bool* is_null), where is_null makes the result NULL
//   absl::StatusOr<Out> f(arguments...) or Out f(arguments...)
//
// A captureless lambda converted to a function pointer, such as Apply<+[](...) {...}>, adapts a
// function that takes fixed arguments or another convention.

namespace bigquery_emulator_duckdb::duckdb_functions {

// How a function's T argument is read from a column, and how a T result is written. A Codec
// that has no Read supports T only as a result.
template <typename T>
struct Codec {
  static T Write(T&& value) { return std::move(value); }
};

template <typename T, auto kRead>
struct ColumnCodec {
  static absl::StatusOr<T> Read(const Arguments& arguments, idx_t column) {
    return (arguments.*kRead)(column);
  }
  static T Write(T&& value) { return std::move(value); }
};

template <>
struct Codec<double> : ColumnCodec<double, &Arguments::Double> {};
template <>
struct Codec<bool> : ColumnCodec<bool, &Arguments::Bool> {};
// An INT64, or a TIME, DATETIME or TIMESTAMP as microseconds.
template <>
struct Codec<int64_t> : ColumnCodec<int64_t, &Arguments::Int> {};
// A DATE as days since the epoch.
template <>
struct Codec<int32_t> : ColumnCodec<int32_t, &Arguments::Date> {};
// A STRING or BYTES.
template <>
struct Codec<std::string> : ColumnCodec<std::string, &Arguments::String> {};

// A string_view argument views the string read for the row. A string_view result, which may
// point into the arguments, is copied before they go.
template <>
struct Codec<std::string_view> {
  static absl::StatusOr<std::string> Read(const Arguments& arguments, idx_t column) {
    return arguments.String(column);
  }
  static std::string Write(std::string_view value) { return std::string(value); }
};

// An ARRAY<INT64> argument views the list read for the row, which has no NULL elements.
template <>
struct Codec<absl::Span<const int64_t>> {
  static absl::StatusOr<std::vector<int64_t>> Read(const Arguments& arguments, idx_t column) {
    return arguments.Ints(column);
  }
};

// A NUMERIC as a DECIMAL(38, 9), whose units are its packed integer.
template <>
struct Codec<googlesql::NumericValue> {
  static absl::StatusOr<googlesql::NumericValue> Read(const Arguments& arguments, idx_t column) {
    return googlesql::NumericValue::FromPackedInt(arguments.Decimal(column));
  }
  static __int128 Write(const googlesql::NumericValue& value) { return value.as_packed_int(); }
};

// A BIGNUMERIC as the VARCHAR of its units; see src/bignumeric.h.
template <>
struct Codec<googlesql::BigNumericValue> {
  static absl::StatusOr<googlesql::BigNumericValue> Read(const Arguments& arguments, idx_t column) {
    return BigNumericFromUnits(arguments.String(column));
  }
  static std::string Write(const googlesql::BigNumericValue& value) {
    return BigNumericUnits(value);
  }
};

// An INTERVAL, which GoogleSQL validates as it reads it.
template <>
struct Codec<googlesql::IntervalValue> {
  static absl::StatusOr<googlesql::IntervalValue> Read(const Arguments& arguments, idx_t column) {
    return IntervalFromDuckDb(
        VectorElement<duckdb_interval>(arguments.Vector(column), arguments.Row()));
  }
  static duckdb_interval Write(const googlesql::IntervalValue& value) {
    return IntervalToDuckDb(value);
  }
};

// An ARRAY result, whose elements are written as T is.
template <typename T>
struct Codec<std::vector<T>> {
  static auto Write(std::vector<T> values) {
    std::vector<decltype(Codec<T>::Write(std::declval<T>()))> out(values.size());
    std::ranges::transform(values, out.begin(),
                           [](T& value) { return Codec<T>::Write(std::move(value)); });
    return out;
  }
};

// What a function's errors do.
enum class Errors : std::uint8_t {
  // Fail the query.
  kFail,
  // Make the result NULL, as SAFE_DIVIDE's do.
  kNull,
  // Make the result NULL when the column after the function's arguments is true, as under
  // SAFE_CAST.
  kNullUnderSafeCast,
};

namespace apply_internal {

// The type of a function's parameter `K` from the end, or void if it has fewer.
template <std::size_t K, typename Parameters, typename = void>
struct FromEnd {
  using type = void;
};

template <std::size_t K, typename... Parameters>
struct FromEnd<K, std::tuple<Parameters...>, std::enable_if_t<(K < sizeof...(Parameters))>> {
  using type = std::tuple_element_t<sizeof...(Parameters) - 1 - K, std::tuple<Parameters...>>;
};

template <typename T>
struct Unwrap {
  using type = T;
};

template <typename T>
struct Unwrap<absl::StatusOr<T>> {
  using type = T;
};

enum class Convention : std::uint8_t { kOutAndError, kOut, kOutAndNull, kReturn };

template <typename Function>
struct Signature;

template <typename R, typename... Parameters>
struct Signature<R (*)(Parameters...)> {
  template <std::size_t K>
  using Last = FromEnd<K, std::tuple<Parameters...>>::type;

  static constexpr Convention kConvention = [] {
    if constexpr (std::is_same_v<R, bool> && std::is_same_v<Last<0>, absl::Status*>) {
      return Convention::kOutAndError;
    } else if constexpr (std::is_same_v<R, absl::Status> && std::is_same_v<Last<0>, bool*> &&
                         std::is_pointer_v<Last<1>>) {
      return Convention::kOutAndNull;
    } else if constexpr (std::is_same_v<R, absl::Status>) {
      static_assert(std::is_pointer_v<Last<0>>, "an absl::Status function needs an out parameter");
      return Convention::kOut;
    } else {
      return Convention::kReturn;
    }
  }();
  // Used by Call's index_sequence; Cppcheck misses these template arguments.
  // cppcheck-suppress unusedStructMember
  static constexpr std::size_t kArity =
      sizeof...(Parameters) -
      (kConvention == Convention::kOutAndError || kConvention == Convention::kOutAndNull
           ? 2
           : (kConvention == Convention::kOut ? 1 : 0));
  template <std::size_t I>
  using Parameter = std::remove_cvref_t<std::tuple_element_t<I, std::tuple<Parameters...>>>;
  using Out =
      std::conditional_t<kConvention == Convention::kReturn, typename Unwrap<R>::type,
                         std::remove_pointer_t<Last<kConvention == Convention::kOut ? 0 : 1>>>;
  using Result = decltype(Codec<std::remove_cvref_t<Out>>::Write(std::declval<Out>()));
};

// The result of `kFunction` for the row's first kArity columns, NULL where its convention says.
template <auto kFunction, std::size_t... I>
auto Call(const Arguments& arguments, std::index_sequence<I...> /*columns*/)
    -> absl::StatusOr<std::optional<typename Signature<decltype(kFunction)>::Result>> {
  using S = Signature<decltype(kFunction)>;
  using OutCodec = Codec<std::remove_cvref_t<typename S::Out>>;
  std::tuple values{Codec<typename S::template Parameter<I>>::Read(arguments, I)...};
  absl::Status status;
  (status.Update(std::get<I>(values).status()), ...);
  if (!status.ok()) {
    return status;
  }
  if constexpr (S::kConvention == Convention::kReturn) {
    auto out = kFunction(*std::get<I>(values)...);
    if constexpr (std::is_same_v<decltype(out), typename S::Out>) {
      return OutCodec::Write(std::move(out));
    } else {
      if (!out.ok()) {
        return out.status();
      }
      return OutCodec::Write(*std::move(out));
    }
  } else {
    typename S::Out out{};
    if constexpr (S::kConvention == Convention::kOutAndError) {
      if (!kFunction(*std::get<I>(values)..., &out, &status)) {
        return status;
      }
    } else if constexpr (S::kConvention == Convention::kOut) {
      if (status = kFunction(*std::get<I>(values)..., &out); !status.ok()) {
        return status;
      }
    } else {
      bool is_null = true;
      if (status = kFunction(*std::get<I>(values)..., &out, &is_null); !status.ok()) {
        return status;
      }
      if (is_null) {
        return std::nullopt;
      }
    }
    return OutCodec::Write(std::move(out));
  }
}

}  // namespace apply_internal

// The GoogleSQL function `kFunction` as a DuckDB scalar function, whose errors do as `kErrors`
// says.
template <auto kFunction, Errors kErrors = Errors::kFail>
void Apply(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  using S = apply_internal::Signature<decltype(kFunction)>;
  EachRow(info, input, output, [](const Arguments& arguments) {
    auto result = apply_internal::Call<kFunction>(arguments, std::make_index_sequence<S::kArity>());
    if (!result.ok() && (kErrors == Errors::kNull ||
                         (kErrors == Errors::kNullUnderSafeCast && arguments.Bool(S::kArity)))) {
      result = std::nullopt;
    }
    return result;
  });
}

}  // namespace bigquery_emulator_duckdb::duckdb_functions
