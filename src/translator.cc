#include "src/translator.h"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bigquery_emulator_duckdb {
namespace {

std::string ToUpper(std::string_view text) {
  std::string result(text);
  for (char& c : result) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

bool IsIdentifierStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool IsIdentifierChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Type names that differ between GoogleSQL and DuckDB. Only applied when the word is used as a
// type (i.e. not immediately followed by "(", where it would be a function call).
const std::unordered_map<std::string, std::string>& TypeNames() {
  static const auto* const kNames = new std::unordered_map<std::string, std::string>{
      {"INT64", "BIGINT"},
      {"FLOAT64", "DOUBLE"},
      {"BOOL", "BOOLEAN"},
      {"STRING", "VARCHAR"},
      {"BYTES", "BLOB"},
      {"NUMERIC", "DECIMAL(38, 9)"},
      {"BIGNUMERIC", "DECIMAL(38, 19)"},
      // BigQuery TIMESTAMP is an absolute instant; DATETIME is a civil time.
      {"TIMESTAMP", "TIMESTAMPTZ"},
      {"DATETIME", "TIMESTAMP"},
  };
  return *kNames;
}

// Function names that differ between GoogleSQL and DuckDB. Only applied when the word is
// immediately followed by "(".
const std::unordered_map<std::string, std::string>& FunctionNames() {
  static const auto* const kNames = new std::unordered_map<std::string, std::string>{
      {"SAFE_CAST", "TRY_CAST"},
  };
  return *kNames;
}

class Translator {
 public:
  explicit Translator(std::string_view input) : input_(input) {}

  std::string Run() {
    Translate(/*stop_at_delimiter=*/false);
    return output_;
  }

 private:
  // Position of the top-level "AS" alias found while translating a STRUCT argument.
  struct AliasPosition {
    size_t output_before_as = 0;  // Output length just before the AS keyword.
    size_t output_after_as = 0;   // Output length just after the AS keyword.
  };

  // Translates until the end of input or, when `stop_at_delimiter` is set, until a "," or ")"
  // at parenthesis depth zero (which is left unconsumed).
  void Translate(bool stop_at_delimiter) {
    int depth = 0;
    while (pos_ < input_.size()) {
      const char c = input_[pos_];
      if (stop_at_delimiter && depth == 0 && (c == ',' || c == ')')) {
        return;
      }
      if (c == '(' || c == '[') {
        ++depth;
      } else if (c == ')' || c == ']') {
        --depth;
      }
      if (c == '`') {
        QuotedIdentifier();
      } else if (c == '\'' || c == '"') {
        StringLiteral(/*raw=*/false, /*bytes=*/false);
      } else if (c == '-' && Peek(1) == '-') {
        LineComment(2);
      } else if (c == '#') {
        LineComment(1);
      } else if (c == '/' && Peek(1) == '*') {
        BlockComment();
      } else if (IsIdentifierStart(c)) {
        Word(stop_at_delimiter && depth == 0);
      } else if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
                 (c == '.' && std::isdigit(static_cast<unsigned char>(Peek(1))) != 0)) {
        Number();
      } else {
        output_ += c;
        ++pos_;
      }
    }
  }

  char Peek(size_t offset) const {
    return pos_ + offset < input_.size() ? input_[pos_ + offset] : '\0';
  }

  bool NextNonSpaceIs(char expected) const {
    size_t i = pos_;
    while (i < input_.size() && std::isspace(static_cast<unsigned char>(input_[i])) != 0) {
      ++i;
    }
    return i < input_.size() && input_[i] == expected;
  }

  // `project.dataset.table` -> "project"."dataset"."table"
  void QuotedIdentifier() {
    ++pos_;  // Opening backtick.
    std::string part;
    auto flush = [&] {
      output_ += '"';
      for (const char c : part) {
        output_ += c;
        if (c == '"') {
          output_ += '"';
        }
      }
      output_ += '"';
      part.clear();
    };
    while (pos_ < input_.size() && input_[pos_] != '`') {
      const char c = input_[pos_++];
      if (c == '\\' && pos_ < input_.size()) {
        part += input_[pos_++];
      } else if (c == '.') {
        flush();
        output_ += '.';
      } else {
        part += c;
      }
    }
    ++pos_;  // Closing backtick.
    flush();
  }

  // Handles '...', "...", '''...''', """...""" and the r/b prefixes. GoogleSQL processes
  // backslash escapes in regular strings, DuckDB only in E'...' strings; raw strings map to
  // plain DuckDB strings, where a backslash is already literal.
  void StringLiteral(bool raw, bool bytes) {
    const char quote = input_[pos_];
    const bool triple = Peek(1) == quote && Peek(2) == quote;
    pos_ += triple ? 3 : 1;
    std::string body;
    bool has_escape = false;
    while (pos_ < input_.size()) {
      const char c = input_[pos_];
      if (c == quote && (!triple || (Peek(1) == quote && Peek(2) == quote))) {
        pos_ += triple ? 3 : 1;
        break;
      }
      if (c == '\\' && !raw && pos_ + 1 < input_.size()) {
        has_escape = true;
        body += c;
        body += input_[pos_ + 1];
        pos_ += 2;
        continue;
      }
      if (c == '\'') {
        body += "''";
      } else {
        body += c;
      }
      ++pos_;
    }
    output_ += has_escape ? "E'" : "'";
    output_ += body;
    output_ += '\'';
    if (bytes) {
      output_ += "::BLOB";
    }
  }

  // GoogleSQL treats every literal with a decimal point as FLOAT64, whereas DuckDB infers a
  // DECIMAL, so "1.5" becomes "1.5::DOUBLE". Literals with an exponent are already DOUBLE.
  void Number() {
    const size_t start = pos_;
    bool has_point = false;
    bool has_exponent = false;
    while (pos_ < input_.size()) {
      const char c = input_[pos_];
      if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
        ++pos_;
      } else if (c == '.' && !has_point && !has_exponent) {
        has_point = true;
        ++pos_;
      } else if ((c == 'e' || c == 'E') && !has_exponent &&
                 (std::isdigit(static_cast<unsigned char>(Peek(1))) != 0 ||
                  ((Peek(1) == '+' || Peek(1) == '-') &&
                   std::isdigit(static_cast<unsigned char>(Peek(2))) != 0))) {
        has_exponent = true;
        pos_ += Peek(1) == '+' || Peek(1) == '-' ? 2 : 1;
      } else {
        break;
      }
    }
    output_ += input_.substr(start, pos_ - start);
    if (has_point && !has_exponent) {
      output_ += "::DOUBLE";
    }
  }

  void LineComment(size_t marker_length) {
    output_ += "--";
    pos_ += marker_length;
    while (pos_ < input_.size() && input_[pos_] != '\n') {
      output_ += input_[pos_++];
    }
  }

  void BlockComment() {
    while (pos_ < input_.size()) {
      if (input_[pos_] == '*' && Peek(1) == '/') {
        output_ += "*/";
        pos_ += 2;
        return;
      }
      output_ += input_[pos_++];
    }
  }

  void Word(bool record_alias) {
    const size_t start = pos_;
    while (pos_ < input_.size() && IsIdentifierChar(input_[pos_])) {
      ++pos_;
    }
    const std::string_view word = input_.substr(start, pos_ - start);
    const std::string upper = ToUpper(word);

    if (record_alias && upper == "AS") {
      alias_ = AliasPosition{output_.size(), output_.size() + word.size()};
    }
    if (upper == "STRUCT" && NextNonSpaceIs('(')) {
      StructConstructor();
      return;
    }
    if ((upper == "ARRAY" || upper == "STRUCT") && NextNonSpaceIs('<')) {
      // Typed constructors such as ARRAY<INT64>[1, 2]: DuckDB infers the type, so the type
      // parameter is dropped.
      SkipTypeParameter();
      if (upper == "STRUCT") {
        StructConstructor();
      }
      return;
    }
    // CURRENT_TIMESTAMP() and friends are functions in GoogleSQL but keywords in DuckDB.
    if ((upper == "CURRENT_TIMESTAMP" || upper == "CURRENT_DATE" || upper == "CURRENT_TIME") &&
        SkipEmptyParens()) {
      output_ += word;
      return;
    }

    // r'...', b'...', rb'...' string prefixes.
    if (upper.size() <= 2 && (Peek(0) == '\'' || Peek(0) == '"') &&
        upper.find_first_not_of("RB") == std::string::npos) {
      StringLiteral(/*raw=*/upper.find('R') != std::string::npos,
                    /*bytes=*/upper.find('B') != std::string::npos);
      return;
    }

    if (NextNonSpaceIs('(')) {
      if (const auto it = FunctionNames().find(upper); it != FunctionNames().end()) {
        output_ += it->second;
        return;
      }
    } else if (const auto it = TypeNames().find(upper); it != TypeNames().end()) {
      output_ += it->second;
      return;
    }
    output_ += word;
  }

  // Consumes "()" when the next non-space characters are exactly that. Returns false otherwise,
  // leaving the input untouched.
  bool SkipEmptyParens() {
    size_t i = pos_;
    while (i < input_.size() && std::isspace(static_cast<unsigned char>(input_[i])) != 0) {
      ++i;
    }
    if (i + 1 < input_.size() && input_[i] == '(' && input_[i + 1] == ')') {
      pos_ = i + 2;
      return true;
    }
    return false;
  }

  // Skips a "<...>" type parameter list, including nested ones.
  void SkipTypeParameter() {
    while (pos_ < input_.size() && input_[pos_] != '<') {
      ++pos_;
    }
    int depth = 0;
    while (pos_ < input_.size()) {
      const char c = input_[pos_++];
      if (c == '<') {
        ++depth;
      } else if (c == '>' && --depth == 0) {
        break;
      }
    }
  }

  // STRUCT(1 AS a, 'x' AS b) -> struct_pack(a := 1, b := 'x')
  void StructConstructor() {
    while (pos_ < input_.size() && input_[pos_] != '(') {
      ++pos_;
    }
    ++pos_;  // Opening parenthesis.
    output_ += "struct_pack(";
    int index = 0;
    while (pos_ < input_.size()) {
      const size_t argument_start = output_.size();
      const std::optional<AliasPosition> outer_alias = alias_;
      alias_.reset();
      Translate(/*stop_at_delimiter=*/true);
      std::string argument = output_.substr(argument_start);
      output_.resize(argument_start);
      std::string name;
      std::string expression;
      if (alias_.has_value()) {
        expression = argument.substr(0, alias_->output_before_as - argument_start);
        name = argument.substr(alias_->output_after_as - argument_start);
      } else {
        expression = argument;
        name = "_field_" + std::to_string(index + 1);
      }
      alias_ = outer_alias;
      output_ += Trim(name) + " := " + Trim(expression);
      ++index;
      if (pos_ < input_.size() && input_[pos_] == ',') {
        output_ += ", ";
        ++pos_;
        continue;
      }
      ++pos_;  // Closing parenthesis.
      break;
    }
    output_ += ')';
  }

  static std::string Trim(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
      return "";
    }
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
  }

  std::string_view input_;
  size_t pos_ = 0;
  std::string output_;
  std::optional<AliasPosition> alias_;
};

}  // namespace

std::string TranslateToDuckDbSql(const FrontendResult& frontend_result) {
  return Translator(frontend_result.sql()).Run();
}

}  // namespace bigquery_emulator_duckdb
