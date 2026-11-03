#include "src/text_encoding.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/strings/match.h"
#include "src/api_error.h"

namespace bigquery_emulator_duckdb {
namespace {

void AppendUtf8(std::string& out, uint32_t code) {
  if (code < 0x80U) {
    out += static_cast<char>(code);
  } else if (code < 0x800U) {
    out += static_cast<char>(0xc0U | (code >> 6U));
    out += static_cast<char>(0x80U | (code & 0x3fU));
  } else if (code < 0x10000U) {
    out += static_cast<char>(0xe0U | (code >> 12U));
    out += static_cast<char>(0x80U | ((code >> 6U) & 0x3fU));
    out += static_cast<char>(0x80U | (code & 0x3fU));
  } else {
    out += static_cast<char>(0xf0U | (code >> 18U));
    out += static_cast<char>(0x80U | ((code >> 12U) & 0x3fU));
    out += static_cast<char>(0x80U | ((code >> 6U) & 0x3fU));
    out += static_cast<char>(0x80U | (code & 0x3fU));
  }
}

constexpr std::pair<std::string_view, TextEncoding> kNames[] = {
    {"UTF-8", TextEncoding::kUtf8},       {"ISO-8859-1", TextEncoding::kIso88591},
    {"UTF-16BE", TextEncoding::kUtf16Be}, {"UTF-16LE", TextEncoding::kUtf16Le},
    {"UTF-32BE", TextEncoding::kUtf32Be}, {"UTF-32LE", TextEncoding::kUtf32Le},
};

[[noreturn]] void RejectSequence(std::string_view encoding, size_t offset) {
  throw ApiError::Unsupported("reading a " + std::string(encoding) +
                              " source with a sequence that is not a character, at byte " +
                              std::to_string(offset));
}

// The code unit of `width` bytes at `offset` of `bytes`, big-endian or not.
uint32_t CodeUnit(std::string_view bytes, size_t offset, size_t width, bool big_endian) {
  uint32_t unit = 0;
  for (size_t i = 0; i < width; ++i) {
    const auto byte =
        static_cast<unsigned char>(bytes.at(offset + (big_endian ? i : width - 1 - i)));
    unit = (unit << 8U) | static_cast<uint32_t>(byte);
  }
  return unit;
}

}  // namespace

std::string_view TextEncodingName(TextEncoding encoding) {
  for (const auto& [name, value] : kNames) {
    if (value == encoding) {
      return name;
    }
  }
  return "UTF-8";
}

std::optional<TextEncoding> ParseTextEncoding(std::string_view name, bool ddl) {
  for (const auto& [canonical, value] : kNames) {
    if (name == canonical || (ddl && absl::EqualsIgnoreCase(name, canonical))) {
      return value;
    }
  }
  // The DDL also spells two of them with an underscore or without a hyphen.
  if (ddl && absl::EqualsIgnoreCase(name, "UTF8")) {
    return TextEncoding::kUtf8;
  }
  if (ddl && absl::EqualsIgnoreCase(name, "ISO_8859_1")) {
    return TextEncoding::kIso88591;
  }
  return std::nullopt;
}

std::string DecodeText(std::string_view bytes, TextEncoding encoding) {
  std::string out;
  switch (encoding) {
    case TextEncoding::kUtf8:
      return std::string(bytes);
    case TextEncoding::kIso88591:
      // Each byte is the code point of its value.
      for (const char c : bytes) {
        AppendUtf8(out, static_cast<unsigned char>(c));
      }
      return out;
    case TextEncoding::kUtf16Be:
    case TextEncoding::kUtf16Le: {
      const bool big_endian = encoding == TextEncoding::kUtf16Be;
      const std::string_view name = big_endian ? "UTF-16BE" : "UTF-16LE";
      if (bytes.size() % 2 != 0) {
        RejectSequence(name, bytes.size() - 1);
      }
      for (size_t i = 0; i < bytes.size(); i += 2) {
        uint32_t code = CodeUnit(bytes, i, 2, big_endian);
        if (code >= 0xdc00 && code <= 0xdfff) {
          RejectSequence(name, i);
        }
        if (code >= 0xd800 && code <= 0xdbff) {
          const uint32_t low = i + 4 <= bytes.size() ? CodeUnit(bytes, i + 2, 2, big_endian) : 0;
          if (low < 0xdc00 || low > 0xdfff) {
            RejectSequence(name, i);
          }
          code = 0x10000U + ((code - 0xd800U) << 10U) + (low - 0xdc00U);
          i += 2;
        }
        // BigQuery skips a byte order mark, of UTF-32 too.
        if (code == 0xfeff && i == 0) {
          continue;
        }
        AppendUtf8(out, code);
      }
      return out;
    }
    case TextEncoding::kUtf32Be:
    case TextEncoding::kUtf32Le: {
      const bool big_endian = encoding == TextEncoding::kUtf32Be;
      const std::string_view name = big_endian ? "UTF-32BE" : "UTF-32LE";
      if (bytes.size() % 4 != 0) {
        RejectSequence(name, bytes.size() - (bytes.size() % 4));
      }
      for (size_t i = 0; i < bytes.size(); i += 4) {
        const uint32_t code = CodeUnit(bytes, i, 4, big_endian);
        if (code == 0xfeff && i == 0) {
          continue;
        }
        if (code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
          RejectSequence(name, i);
        }
        AppendUtf8(out, code);
      }
      return out;
    }
  }
  return std::string(bytes);
}

}  // namespace bigquery_emulator_duckdb
