#include "src/googlesql_text_analysis/search.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/value.h"
#include "src/googlesql_text_analysis/text_analysis.h"

namespace googlesql_text_analysis {
namespace {

// The characters a single term escapes with a backslash.
constexpr std::string_view kReserved = R"([]<>(){}|!'"*&?+/:=-\~^)";

// Words a query must enclose in backticks besides the operators AND and OR.
constexpr auto kReservedWords = std::to_array<std::string_view>({"NOT", "IN", "NEAR"});

struct Term {
  enum class Kind : std::uint8_t {
    // Every token appears in the field.
    kTokens,
    // The tokens appear next to each other, in order.
    kPhrase,
    // The text appears between delimiters, as it is.
    kExact,
  };
  Kind kind = Kind::kTokens;
  std::vector<std::string> tokens;
  std::string text;
};

struct Node {
  // kEmpty is a term without search tokens, or a combination of only such terms, which AND and
  // OR leave out.
  enum class Op : std::uint8_t { kTerm, kEmpty, kAnd, kOr };
  Op op = Op::kTerm;
  Term term;
  std::vector<Node> children;
};

Node TermNode(Term term) {
  Node node;
  node.term = std::move(term);
  return node;
}

Node OpNode(Node::Op op) {
  Node node;
  node.op = op;
  return node;
}

// The tokens of BigQuery's search query grammar, an ANTLR grammar whose error messages these
// reproduce.
struct Token {
  enum class Kind : std::uint8_t {
    kOpen,
    kClose,
    kSpace,
    kAnd,
    kOr,
    kBacktick,
    kPhrase,
    kSingle,
    kEof,
  };
  Kind kind = Kind::kEof;
  // The position in code points, as ANTLR counts it.
  std::size_t position = 0;
  // The text as the query spells it, and the term it stands for.
  std::string text;
  std::string value;
};

using Kinds = std::vector<Token::Kind>;

absl::Status ParserError(std::string_view message) {
  return absl::OutOfRangeError(absl::StrCat("Search query parser error: ", message));
}

bool IsSpace(char c) { return absl::ascii_isspace(static_cast<unsigned char>(c)); }

bool IsReserved(char c) { return kReserved.find(c) != std::string_view::npos; }

std::size_t CodePoints(std::string_view text) {
  return std::ranges::count_if(
      text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; });
}

template <typename Range, typename Value>
bool Contains(const Range& range, const Value& value) {
  return std::ranges::find(range, value) != std::ranges::end(range);
}

// Only LOG_ANALYZER's queries have backtick terms; PATTERN_ANALYZER's single terms include
// backticks, which its patterns leave out of the tokens.
class Lexer {
 public:
  Lexer(std::string_view query, bool backticks) : query_(query), backticks_(backticks) {}

  absl::StatusOr<std::vector<Token>> Tokens() {
    std::vector<Token> tokens;
    while (i_ < query_.size()) {
      const std::size_t begin = i_;
      const char c = query_.at(i_);
      Token token{.kind = Token::Kind::kSingle, .position = Position(), .text = {}, .value = {}};
      if (IsSpace(c)) {
        token.kind = Token::Kind::kSpace;
        while (i_ < query_.size() && IsSpace(query_.at(i_))) {
          ++i_;
        }
      } else if (c == '(' || c == ')') {
        token.kind = c == '(' ? Token::Kind::kOpen : Token::Kind::kClose;
        ++i_;
      } else if (c == '"' || (c == '`' && backticks_)) {
        token.kind = c == '"' ? Token::Kind::kPhrase : Token::Kind::kBacktick;
        GOOGLESQL_ASSIGN_OR_RETURN(token.value, Quoted(c));
      } else {
        GOOGLESQL_ASSIGN_OR_RETURN(token.value, Single());
        const std::string_view text = query_.substr(begin, i_ - begin);
        if (text == "AND") {
          token.kind = Token::Kind::kAnd;
        } else if (text == "OR") {
          token.kind = Token::Kind::kOr;
        }
      }
      token.text = std::string(query_.substr(begin, i_ - begin));
      tokens.push_back(std::move(token));
    }
    tokens.push_back({.kind = Token::Kind::kEof, .position = Position(), .text = {}, .value = {}});
    return tokens;
  }

 private:
  [[nodiscard]] std::size_t Position() const { return CodePoints(query_.substr(0, i_)); }

  // ANTLR's error for the characters [begin, end) that start no token.
  [[nodiscard]] absl::Status RecognitionError(std::size_t begin, std::size_t end) const {
    return ParserError(absl::StrCat("error at position ", CodePoints(query_.substr(0, begin)),
                                    ": token recognition error at: '",
                                    query_.substr(begin, end - begin), "'"));
  }

  // A phrase or backtick term, where a backslash escapes only the quote. It has at least one
  // character.
  absl::StatusOr<std::string> Quoted(char quote) {
    const std::size_t begin = i_;
    std::string value;
    for (std::size_t j = begin + 1; j < query_.size(); ++j) {
      if (query_.at(j) == '\\' && j + 1 < query_.size() && query_.at(j + 1) == quote) {
        value += quote;
        ++j;
      } else if (query_.at(j) == quote) {
        if (value.empty()) {
          return RecognitionError(begin, j + 1);
        }
        i_ = j + 1;
        return value;
      } else {
        value += query_.at(j);
      }
    }
    return RecognitionError(begin, query_.size());
  }

  // A single term, which a backslash escapes reserved characters in.
  absl::StatusOr<std::string> Single() {
    std::string value;
    while (i_ < query_.size()) {
      const char c = query_.at(i_);
      if (IsSpace(c) || c == '(' || c == ')' || c == '"' || (c == '`' && backticks_)) {
        break;
      }
      if (c == '\\') {
        if (i_ + 1 == query_.size() || !IsReserved(query_.at(i_ + 1))) {
          return RecognitionError(i_, std::min(i_ + 2, query_.size()));
        }
        value += query_.at(i_ + 1);
        i_ += 2;
      } else if (IsReserved(c)) {
        return RecognitionError(i_, i_ + 1);
      } else {
        value += c;
        ++i_;
      }
    }
    return value;
  }

  std::string_view query_;
  bool backticks_;
  std::size_t i_ = 0;
};

constexpr std::array kTermStart = {
    Token::Kind::kOpen,
    Token::Kind::kBacktick,
    Token::Kind::kPhrase,
    Token::Kind::kSingle,
};

// Recursive descent over the grammar of the search query rules, where OR binds more loosely than
// AND and whitespace, and both operators need whitespace on either side.
class Parser {
 public:
  Parser(const std::vector<Token>& tokens, const Analyzer& analyzer)
      : tokens_(tokens), analyzer_(analyzer) {}

  absl::StatusOr<Node> Parse() {
    GOOGLESQL_ASSIGN_OR_RETURN(Node root, Or(0));
    if (Current().kind != Token::Kind::kEof) {
      return Expected({Token::Kind::kEof, Token::Kind::kSpace}, "{<EOF>, WhiteSpace}");
    }
    return root;
  }

 private:
  [[nodiscard]] const Token& Current() const { return tokens_.at(next_); }

  [[nodiscard]] const Token& Following() const {
    return tokens_.at(std::min(next_ + 1, tokens_.size() - 1));
  }

  [[nodiscard]] std::string_view Display() const {
    return Current().kind == Token::Kind::kEof ? "<EOF>" : std::string_view(Current().text);
  }

  // ANTLR's error at the current token: extraneous when the token after it is one of `expected`,
  // so that deleting it would recover, otherwise mismatched.
  [[nodiscard]] absl::Status Expected(const Kinds& expected, std::string_view description) const {
    const bool extraneous =
        Current().kind != Token::Kind::kEof && Contains(expected, Following().kind);
    return ParserError(absl::StrCat("error at position ", Current().position, ": ",
                                    extraneous ? "extraneous" : "mismatched", " input '", Display(),
                                    "' expecting ", description));
  }

  // The whitespace after AND and OR, which ANTLR reports missing when a term follows instead.
  absl::Status RequireSpace() {
    if (Current().kind == Token::Kind::kSpace) {
      ++next_;
      return absl::OkStatus();
    }
    if (Following().kind != Token::Kind::kSpace && Contains(kTermStart, Current().kind)) {
      return ParserError(absl::StrCat("error at position ", Current().position,
                                      ": missing WhiteSpace at '", Display(), "'"));
    }
    return Expected({Token::Kind::kSpace}, "WhiteSpace");
  }

  absl::StatusOr<Node> Or(int depth) {
    GOOGLESQL_ASSIGN_OR_RETURN(Node left, And(depth));
    Node node = OpNode(Node::Op::kOr);
    node.children.push_back(std::move(left));
    while (Current().kind == Token::Kind::kSpace && Following().kind == Token::Kind::kOr) {
      next_ += 2;
      GOOGLESQL_RETURN_IF_ERROR(RequireSpace());
      GOOGLESQL_ASSIGN_OR_RETURN(Node right, And(depth));
      node.children.push_back(std::move(right));
    }
    if (node.children.size() == 1) {
      return std::move(node.children.front());
    }
    if (std::ranges::all_of(node.children,
                            [](const Node& child) { return child.op == Node::Op::kEmpty; })) {
      return OpNode(Node::Op::kEmpty);
    }
    return node;
  }

  absl::StatusOr<Node> And(int depth) {
    Node node = OpNode(Node::Op::kAnd);
    GOOGLESQL_ASSIGN_OR_RETURN(Node first, Primary(depth));
    node.children.push_back(std::move(first));
    while (true) {
      const Token::Kind kind = Current().kind;
      if (kind == Token::Kind::kSpace) {
        const Token::Kind following = Following().kind;
        if (following == Token::Kind::kOr) {
          break;
        }
        ++next_;
        if (following == Token::Kind::kEof || (following == Token::Kind::kClose && depth > 0)) {
          break;
        }
        if (following == Token::Kind::kAnd) {
          ++next_;
          GOOGLESQL_RETURN_IF_ERROR(RequireSpace());
        }
      } else if (kind == Token::Kind::kEof || (kind == Token::Kind::kClose && depth > 0)) {
        break;
      } else if (depth > 0) {
        return Expected({Token::Kind::kClose, Token::Kind::kSpace}, "{')', WhiteSpace}");
      } else {
        return Expected({Token::Kind::kEof, Token::Kind::kSpace}, "{<EOF>, WhiteSpace}");
      }
      GOOGLESQL_ASSIGN_OR_RETURN(Node right, Primary(depth));
      node.children.push_back(std::move(right));
    }
    std::erase_if(node.children, [](const Node& child) { return child.op == Node::Op::kEmpty; });
    if (node.children.empty()) {
      return OpNode(Node::Op::kEmpty);
    }
    if (node.children.size() == 1) {
      return std::move(node.children.front());
    }
    return node;
  }

  absl::StatusOr<Node> Primary(int depth) {
    if (Current().kind == Token::Kind::kSpace) {
      ++next_;
    }
    // Whitespace before a term is one token, so a term must follow it.
    const bool spaced = next_ > 0 && tokens_.at(next_ - 1).kind == Token::Kind::kSpace;
    const Token& token = Current();
    switch (token.kind) {
      case Token::Kind::kOpen: {
        ++next_;
        GOOGLESQL_ASSIGN_OR_RETURN(Node node, Or(depth + 1));
        if (Current().kind != Token::Kind::kClose) {
          return Expected({Token::Kind::kClose, Token::Kind::kSpace}, "{')', WhiteSpace}");
        }
        ++next_;
        return node;
      }
      case Token::Kind::kBacktick:
        ++next_;
        return TermNode({.kind = Term::Kind::kExact, .tokens = {}, .text = token.value});
      case Token::Kind::kSingle:
      case Token::Kind::kPhrase: {
        ++next_;
        if (token.kind == Token::Kind::kSingle &&
            Contains(kReservedWords, std::string_view(token.text))) {
          return ParserError(
              absl::StrCat("Search query contains the reserved keyword '", token.text, "'."));
        }
        GOOGLESQL_ASSIGN_OR_RETURN(std::vector<std::string> tokens,
                                   Analyze(analyzer_, token.value));
        if (tokens.empty()) {
          return OpNode(Node::Op::kEmpty);
        }
        return TermNode({
            .kind = token.kind == Token::Kind::kPhrase ? Term::Kind::kPhrase : Term::Kind::kTokens,
            .tokens = std::move(tokens),
            .text = {},
        });
      }
      default: {
        if (spaced) {
          return Expected(Kinds(kTermStart.begin(), kTermStart.end()),
                          "{'(', BacktickToken, DoubleQuoteToken, SingleTermToken}");
        }
        Kinds expected(kTermStart.begin(), kTermStart.end());
        expected.push_back(Token::Kind::kSpace);
        return Expected(expected,
                        "{'(', BacktickToken, DoubleQuoteToken, SingleTermToken, WhiteSpace}");
      }
    }
  }

  const std::vector<Token>& tokens_;
  const Analyzer& analyzer_;
  std::size_t next_ = 0;
};

// One field's text and, once a term needs them, its tokens.
class Field {
 public:
  Field(const Analyzer& analyzer, std::string_view text) : analyzer_(analyzer), text_(text) {}

  absl::StatusOr<bool> Matches(const Term& term) {
    if (term.kind == Term::Kind::kExact) {
      for (std::size_t at = text_.find(term.text); at != std::string_view::npos;
           at = text_.find(term.text, at + 1)) {
        if (BetweenDelimiters(analyzer_, text_, at, at + term.text.size())) {
          return true;
        }
      }
      return false;
    }
    if (!tokens_) {
      GOOGLESQL_ASSIGN_OR_RETURN(tokens_, Analyze(analyzer_, std::string(text_)));
      distinct_ = std::set<std::string_view>(tokens_->begin(), tokens_->end());
    }
    if (term.kind == Term::Kind::kPhrase) {
      return !std::ranges::search(*tokens_, term.tokens).empty();
    }
    return std::ranges::all_of(term.tokens,
                               [&](const std::string& token) { return distinct_.contains(token); });
  }

 private:
  const Analyzer& analyzer_;
  std::string_view text_;
  std::optional<std::vector<std::string>> tokens_;
  std::set<std::string_view> distinct_;
};

absl::StatusOr<bool> Evaluate(const Node& node, Field& field) {
  switch (node.op) {
    case Node::Op::kTerm:
      return field.Matches(node.term);
    case Node::Op::kEmpty:
      return false;
    case Node::Op::kAnd:
      for (const Node& child : node.children) {
        GOOGLESQL_ASSIGN_OR_RETURN(const bool matches, Evaluate(child, field));
        if (!matches) {
          return false;
        }
      }
      return true;
    case Node::Op::kOr:
      for (const Node& child : node.children) {
        GOOGLESQL_ASSIGN_OR_RETURN(const bool matches, Evaluate(child, field));
        if (matches) {
          return true;
        }
      }
      return false;
  }
  return false;
}

bool HasType(const googlesql::Type* type, bool (googlesql::Type::*is)() const) {
  if ((type->*is)()) {
    return true;
  }
  if (type->IsArray()) {
    return HasType(type->AsArray()->element_type(), is);
  }
  if (type->IsStruct()) {
    return std::ranges::any_of(type->AsStruct()->fields(),
                               [is](const auto& field) { return HasType(field.type, is); });
  }
  return false;
}

bool HasText(const googlesql::Type* type) {
  return HasType(type, &googlesql::Type::IsString) || HasType(type, &googlesql::Type::IsJson);
}

}  // namespace

struct SearchQuery {
  std::shared_ptr<const Analyzer> analyzer;
  // NO_OP_ANALYZER's query, which a field equals exactly.
  std::optional<std::string> whole;
  Node root;
};

absl::StatusOr<std::shared_ptr<const SearchQuery>> ParseSearchQuery(
    std::string_view query, std::shared_ptr<const Analyzer> analyzer) {
  auto search = std::make_shared<SearchQuery>();
  if (KindOf(*analyzer) == AnalyzerKind::kNoOp) {
    search->whole = std::string(query);
    search->analyzer = std::move(analyzer);
    return search;
  }
  GOOGLESQL_ASSIGN_OR_RETURN(const std::vector<Token> tokens,
                             Lexer(query, KindOf(*analyzer) == AnalyzerKind::kLog).Tokens());
  GOOGLESQL_ASSIGN_OR_RETURN(search->root, Parser(tokens, *analyzer).Parse());
  if (search->root.op == Node::Op::kEmpty) {
    return absl::OutOfRangeError(absl::StrCat("Search query '", query, "' results in 0 tokens."));
  }
  search->analyzer = std::move(analyzer);
  return search;
}

absl::StatusOr<bool> SearchField(const SearchQuery& query, std::string_view text) {
  if (query.whole) {
    return text == *query.whole;
  }
  Field field(*query.analyzer, text);
  return Evaluate(query.root, field);
}

absl::StatusOr<JsonScope> ParseJsonScope(std::string_view scope) {
  if (scope == "JSON_VALUES") {
    return JsonScope::kValues;
  }
  if (scope == "JSON_KEYS") {
    return JsonScope::kKeys;
  }
  if (scope == "JSON_KEYS_AND_VALUES") {
    return JsonScope::kKeysAndValues;
  }
  return absl::OutOfRangeError(absl::StrCat("Invalid json_scope value '", scope, "'."));
}

namespace {

struct Outcome {
  bool matched = false;
  bool null = false;
};

class Searcher {
 public:
  Searcher(const SearchQuery& query, JsonScope scope) : query_(query), scope_(scope) {}

  // Searches each STRING field of `data`, until one matches. A NULL value with STRING fields
  // makes them NULL.
  absl::Status Fields(const googlesql::Value& data) {
    const googlesql::Type* type = data.type();
    if (!HasText(type)) {
      return absl::OkStatus();
    }
    if (data.is_null()) {
      outcome_.null = true;
      return absl::OkStatus();
    }
    if (type->IsString()) {
      return Text(data.string_value());
    }
    if (type->IsJson()) {
      return Json(data.json_value());
    }
    const int children = type->IsArray() ? data.num_elements() : data.num_fields();
    for (int i = 0; i < children && !outcome_.matched; ++i) {
      GOOGLESQL_RETURN_IF_ERROR(Fields(type->IsArray() ? data.element(i) : data.field(i)));
    }
    return absl::OkStatus();
  }

  [[nodiscard]] const Outcome& Result() const { return outcome_; }

 private:
  absl::Status Text(std::string_view text) {
    GOOGLESQL_ASSIGN_OR_RETURN(outcome_.matched, SearchField(query_, text));
    return absl::OkStatus();
  }

  // Each JSON string value or object key in the scope is a field of its own.
  absl::Status Json(googlesql::JSONValueConstRef json) {
    if (json.IsString()) {
      return scope_ == JsonScope::kKeys ? absl::OkStatus() : Text(json.GetString());
    }
    if (json.IsObject()) {
      for (const auto& [key, value] : json.GetMembers()) {
        if (scope_ != JsonScope::kValues) {
          GOOGLESQL_RETURN_IF_ERROR(Text(key));
        }
        if (!outcome_.matched) {
          GOOGLESQL_RETURN_IF_ERROR(Json(value));
        }
        if (outcome_.matched) {
          break;
        }
      }
    } else if (json.IsArray()) {
      for (const auto element : json.GetArrayElements()) {
        GOOGLESQL_RETURN_IF_ERROR(Json(element));
        if (outcome_.matched) {
          break;
        }
      }
    }
    return absl::OkStatus();
  }

  const SearchQuery& query_;
  JsonScope scope_;
  Outcome outcome_;
};

}  // namespace

absl::StatusOr<std::optional<bool>> Search(const googlesql::Value& data, const SearchQuery& query,
                                           JsonScope scope) {
  Searcher searcher(query, scope);
  GOOGLESQL_RETURN_IF_ERROR(searcher.Fields(data));
  if (searcher.Result().matched) {
    return true;
  }
  if (searcher.Result().null) {
    return std::nullopt;
  }
  return false;
}

}  // namespace googlesql_text_analysis
