#include "src/googlesql_functions/format.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "googlesql/public/functions/string_format.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/value.h"

namespace googlesql_functions {
namespace {

// Whether `value` is or holds a STRING with a NUL.
bool HasNul(const googlesql::Value& value) {
  if (value.is_null()) {
    return false;
  }
  switch (value.type_kind()) {
    case googlesql::TYPE_STRING:
      return value.string_value().find('\0') != std::string::npos;
    case googlesql::TYPE_ARRAY:
      for (int i = 0; i < value.num_elements(); ++i) {
        if (HasNul(value.element(i))) {
          return true;
        }
      }
      return false;
    case googlesql::TYPE_STRUCT:
      for (int i = 0; i < value.num_fields(); ++i) {
        if (HasNul(value.field(i))) {
          return true;
        }
      }
      return false;
    default:
      return false;
  }
}

// GoogleSQL's FORMAT, preserving the sign of floating-point zero when `signed_zero` is set, as
// BigQuery does.
absl::Status StringFormat(std::string_view pattern, const std::vector<googlesql::Value>& values,
                          bool signed_zero, std::string* out, bool* is_null) {
  return googlesql::functions::StringFormatUtf8(pattern, values, googlesql::PRODUCT_EXTERNAL, out,
                                                is_null, /*canonicalize_zero=*/!signed_zero,
                                                /*use_external_float32=*/true);
}

// A FORMAT pattern and its values, rewritten by AdaptFormat.
struct AdaptedFormat {
  std::string pattern;
  std::vector<googlesql::Value> values;
};

// Rewrites the specifiers whose output from GoogleSQL differs from BigQuery's, and their values,
// so that GoogleSQL prints BigQuery's result; nullopt when nothing changes. The scan follows
// GoogleSQL's "%[flags][width][.precision]specifier" and assigns arguments as it does. A
// specifier GoogleSQL rejects, or whose arguments are missing, is copied as is so that GoogleSQL
// reports it.
std::optional<AdaptedFormat> AdaptFormat(std::string_view pattern,
                                         const std::vector<googlesql::Value>& values,
                                         bool signed_zero) {
  // GoogleSQL's limit on the output width, which also caps an integer's precision.
  constexpr int64_t kMaxPrecision = 1048576;
  constexpr std::string_view kDigits = "0123456789";
  AdaptedFormat adapted{.pattern = "", .values = values};
  bool changed = false;
  size_t next_arg = 0;
  size_t position = 0;
  while (position < pattern.size()) {
    const size_t percent = pattern.find('%', position);
    if (percent == std::string_view::npos) {
      adapted.pattern.append(pattern.substr(position));
      break;
    }
    adapted.pattern.append(pattern.substr(position, percent - position));
    size_t cursor = percent + 1;
    const auto span = [&](std::string_view chars) {
      const size_t begin = cursor;
      while (cursor < pattern.size() && chars.find(pattern.at(cursor)) != std::string_view::npos) {
        ++cursor;
      }
      return std::string(pattern.substr(begin, cursor - begin));
    };
    const auto star = [&] {
      const bool found = cursor < pattern.size() && pattern.at(cursor) == '*';
      cursor += found ? 1 : 0;
      return found;
    };
    std::string flags = span("-+ #'0");
    const std::string width = star() ? "*" : span(kDigits);
    std::optional<std::string> precision;
    if (cursor < pattern.size() && pattern.at(cursor) == '.') {
      ++cursor;
      precision = star() ? "*" : span(kDigits);
    }
    if (cursor == pattern.size() || pattern.at(cursor) == '\n') {
      adapted.pattern.append(pattern.substr(percent));
      break;
    }
    char conversion = pattern.at(cursor++);
    position = cursor;
    std::optional<size_t> precision_arg;
    if (conversion != '%') {
      next_arg += width == "*" ? 1 : 0;
      if (precision == "*") {
        precision_arg = next_arg++;
      }
    }
    const size_t value_arg = next_arg++;
    if (conversion == '%' || value_arg >= values.size() || values.at(value_arg).is_null()) {
      adapted.pattern.append(pattern.substr(percent, cursor - percent));
      continue;
    }
    const googlesql::Value& value = values.at(value_arg);
    std::string text;
    bool is_null = false;
    if (conversion == 't' && HasNul(value) &&
        StringFormat("%t", {value}, signed_zero, &text, &is_null).ok()) {
      // BigQuery ends the text of %t at the first NUL of a STRING, also of one in an ARRAY or a
      // STRUCT, as GoogleSQL does for %s. A value that fails here fails below as well.
      adapted.values.at(value_arg) = googlesql::Value::String(text.substr(0, text.find('\0')));
      changed = true;
    } else if (std::string_view("dioxX").find(conversion) != std::string_view::npos &&
               value.type_kind() == googlesql::TYPE_INT64) {
      // The explicit precision, or nullopt when it is absent, negative, NULL or above the limit.
      std::optional<int64_t> digits;
      if (precision_arg.has_value()) {
        const googlesql::Value& argument = values.at(*precision_arg);
        if (argument.type_kind() == googlesql::TYPE_INT64 && !argument.is_null() &&
            argument.int64_value() >= 0 && argument.int64_value() <= kMaxPrecision) {
          digits = argument.int64_value();
        }
      } else if (precision.has_value()) {
        int64_t parsed = 0;
        const auto [end, error] =
            std::from_chars(precision->data(), precision->data() + precision->size(), parsed);
        if (error == std::errc() && parsed <= kMaxPrecision) {
          digits = parsed;
        }
      }
      if (digits.has_value()) {
        // An explicit precision disables zero padding.
        std::erase(flags, '0');
        const bool grouping = flags.find('\'') != std::string::npos;
        if (grouping && value.int64_value() == 0 && *digits == 0) {
          // A grouped zero at precision zero prints only its sign, without digits or prefix.
          const std::string sign = flags.find('+') != std::string::npos   ? "+"
                                   : flags.find(' ') != std::string::npos ? " "
                                                                          : "";
          flags = flags.find('-') != std::string::npos ? "-" : "";
          conversion = 's';
          adapted.values.at(value_arg) = googlesql::Value::String(sign);
          if (precision_arg.has_value()) {
            adapted.values.at(*precision_arg) = googlesql::Value::Int64(1);
          } else {
            precision.reset();
          }
          changed = true;
        } else if (grouping) {
          // BigQuery counts grouping separators toward the precision; each group after the
          // first takes one more character than it has digits.
          const int64_t group = conversion == 'd' || conversion == 'i' ? 3 : 4;
          const int64_t counted = *digits - ((*digits - 1) / (group + 1));
          if (precision_arg.has_value()) {
            adapted.values.at(*precision_arg) = googlesql::Value::Int64(counted);
            changed = true;
          } else {
            precision = std::to_string(counted);
          }
        }
      }
    }
    adapted.pattern.append("%").append(flags).append(width);
    if (precision.has_value()) {
      adapted.pattern.append(".").append(*precision);
    }
    adapted.pattern.push_back(conversion);
  }
  if (!changed && adapted.pattern == pattern) {
    return std::nullopt;
  }
  return adapted;
}

}  // namespace

// FORMAT, including BigQuery specifier adaptations and errors quoting the original pattern.
absl::StatusOr<std::optional<std::string>> Format(std::string_view pattern,
                                                  const std::vector<googlesql::Value>& arguments,
                                                  const FormatOptions& options) {
  std::vector<googlesql::Value> values = arguments;
  if (options.unsigned_nan) {
    for (auto& value : values) {
      if (!value.is_null() && value.type()->IsDouble() && std::isnan(value.double_value())) {
        value = googlesql::Value::Double(std::numeric_limits<double>::quiet_NaN());
      }
    }
  }
  const std::optional<AdaptedFormat> adapted =
      options.adapt_specifiers ? AdaptFormat(pattern, values, options.signed_zero) : std::nullopt;
  std::string out;
  bool is_null = false;
  const auto format = [&](std::string_view format_pattern,
                          const std::vector<googlesql::Value>& format_values) {
    return StringFormat(format_pattern, format_values, options.signed_zero, &out, &is_null);
  };
  absl::Status status =
      adapted.has_value() ? format(adapted->pattern, adapted->values) : format(pattern, values);
  if (!status.ok() && adapted.has_value()) {
    // Report an error as for the format the query gave, whose text the message quotes.
    const absl::Status original = format(pattern, values);
    if (!original.ok()) {
      status = original;
    }
  }
  if (!status.ok()) {
    return status;
  }
  return is_null ? std::nullopt : std::optional<std::string>(out);
}

}  // namespace googlesql_functions
