#include "src/translator.h"

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

#include "googlesql/parser/parse_tree.h"
#include "googlesql/parser/unparser.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/type.h"

namespace bigquery_emulator_duckdb {
namespace {

std::string ToUpper(std::string_view text) {
  std::string result(text);
  for (char& c : result) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

// Type names that differ between GoogleSQL and DuckDB.
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

// Maps a GoogleSQL type name onto its DuckDB spelling, keeping the original name when the two
// agree. `has_type_parameters` drops the default parameters of the mapped name, because an
// explicit parameter list follows and replaces them.
std::string MapTypeName(std::string_view name, bool has_type_parameters) {
  const auto it = TypeNames().find(ToUpper(name));
  if (it == TypeNames().end()) {
    return std::string(name);
  }
  return has_type_parameters ? it->second.substr(0, it->second.find('(')) : it->second;
}

// Functions that GoogleSQL spells with empty parentheses and DuckDB spells as a bare keyword.
bool IsBareKeywordFunction(std::string_view upper_name) {
  return upper_name == "CURRENT_TIMESTAMP" || upper_name == "CURRENT_DATE" ||
         upper_name == "CURRENT_TIME";
}

// '...' with the DuckDB escaping rules: a single quote is doubled and nothing else is special.
std::string QuoteString(std::string_view value) {
  std::string result = "'";
  for (const char c : value) {
    result += c;
    if (c == '\'') {
      result += c;
    }
  }
  result += '\'';
  return result;
}

std::string ToHex(std::string_view value) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() * 2);
  for (const char c : value) {
    const auto byte = static_cast<unsigned char>(c);
    result += kDigits[byte >> 4U];
    result += kDigits[byte & 0x0FU];
  }
  return result;
}

// A backtick quoted GoogleSQL identifier holds a whole path in BigQuery: `project.dataset.table`
// is one identifier that names three objects, so it becomes "project"."dataset"."table".
std::string QuotePath(std::string_view name) {
  std::string result;
  result += '"';
  for (const char c : name) {
    if (c == '.') {
      result += "\".\"";
    } else {
      result += c;
      if (c == '"') {
        result += c;
      }
    }
  }
  result += '"';
  return result;
}

// Unparses a GoogleSQL AST as DuckDB SQL: the base class knows how to print GoogleSQL, and the
// overrides below replace the constructs whose spelling differs in DuckDB.
class DuckDbUnparser : public googlesql::parser::Unparser {
 public:
  explicit DuckDbUnparser(std::string* unparsed) : Unparser(unparsed) {}

  void visitASTIdentifier(const googlesql::ASTIdentifier* node, void* data) override {
    if (node->is_quoted()) {
      print(QuotePath(node->GetAsStringView()));
    } else {
      print(node->GetAsStringView());
    }
  }

  // GoogleSQL literals are already unescaped by the parser, so the value only has to be quoted
  // the way DuckDB expects. This covers "...", '''...''' and the r prefix in one place.
  void visitASTStringLiteral(const googlesql::ASTStringLiteral* node, void* data) override {
    print(QuoteString(node->string_value()));
  }

  // Bytes may contain quotes, NUL and non-UTF-8 sequences, none of which survive a string
  // literal, so they go through their hex encoding.
  void visitASTBytesLiteral(const googlesql::ASTBytesLiteral* node, void* data) override {
    print("from_hex(" + QuoteString(ToHex(node->bytes_value())) + ")");
  }

  // GoogleSQL treats every literal with a decimal point as FLOAT64, whereas DuckDB infers a
  // DECIMAL, so the type is pinned down explicitly.
  void visitASTFloatLiteral(const googlesql::ASTFloatLiteral* node, void* data) override {
    print(std::string(node->image()) + "::DOUBLE");
  }

  void visitASTSimpleType(const googlesql::ASTSimpleType* node, void* data) override {
    print(MapTypeName(node->type_name()->ToIdentifierPathString(),
                      node->type_parameters() != nullptr));
    if (node->type_parameters() != nullptr) {
      node->type_parameters()->Accept(this, data);
    }
    if (node->collate() != nullptr) {
      node->collate()->Accept(this, data);
    }
  }

  void visitASTArrayType(const googlesql::ASTArrayType* node, void* data) override {
    node->element_type()->Accept(this, data);
    print("[]");
  }

  void visitASTStructType(const googlesql::ASTStructType* node, void* data) override {
    print("STRUCT(");
    UnparseVectorWithSeparator(node->struct_fields(), data, ",");
    print(")");
  }

  // A column type in DDL is an ASTColumnSchema rather than an ASTType, and the base class
  // prints the rest of the column (parameters, collation, default, options) through a private
  // helper. Rather than reimplementing that tail, the type name node is remembered here and
  // rewritten when the base class walks into it.
  void visitASTSimpleColumnSchema(const googlesql::ASTSimpleColumnSchema* node,
                                  void* data) override {
    column_type_name_ = node->type_name();
    column_type_has_parameters_ = node->type_parameters() != nullptr;
    Unparser::visitASTSimpleColumnSchema(node, data);
    column_type_name_ = nullptr;
  }

  void visitASTPathExpression(const googlesql::ASTPathExpression* node, void* data) override {
    if (node == column_type_name_) {
      print(MapTypeName(node->ToIdentifierPathString(), column_type_has_parameters_));
      return;
    }
    Unparser::visitASTPathExpression(node, data);
  }

  // ARRAY<T> / STRUCT<...> column types. The column tail that the base class would print after
  // them is dropped along the way; defaults and options on a nested column type are not
  // supported yet.
  void visitASTArrayColumnSchema(const googlesql::ASTArrayColumnSchema* node, void* data) override {
    node->element_schema()->Accept(this, data);
    print("[]");
  }

  void visitASTStructColumnSchema(const googlesql::ASTStructColumnSchema* node,
                                  void* data) override {
    print("STRUCT(");
    UnparseVectorWithSeparator(node->struct_fields(), data, ",");
    print(")");
  }

  // TIMESTAMP '2024-01-01 00:00:00' and friends name their type as a keyword, not as a type
  // node, so the same mapping is applied here.
  void visitASTDateOrTimeLiteral(const googlesql::ASTDateOrTimeLiteral* node, void* data) override {
    print(MapTypeName(
        googlesql::Type::TypeKindToString(node->type_kind(), googlesql::PRODUCT_INTERNAL),
        /*has_type_parameters=*/false));
    node->string_literal()->Accept(this, data);
  }

  void visitASTArrayConstructor(const googlesql::ASTArrayConstructor* node, void* data) override {
    // DuckDB infers the element type of a list literal and has no ARRAY<T>[...] spelling.
    print("[");
    UnparseVectorWithSeparator(node->elements(), data, ",");
    print("]");
  }

  // STRUCT(1 AS a, 'x' AS b) -> struct_pack(a := 1, b := 'x')
  void visitASTStructConstructorWithKeyword(const googlesql::ASTStructConstructorWithKeyword* node,
                                            void* data) override {
    print("struct_pack(");
    const auto fields = node->fields();
    for (size_t i = 0; i < fields.size(); ++i) {
      if (i > 0) {
        print(",");
      }
      const googlesql::ASTStructConstructorArg* field = fields[i];
      const std::string name = field->alias() == nullptr
                                   ? "_field_" + std::to_string(i + 1)
                                   : std::string(field->alias()->identifier()->GetAsStringView());
      print(name + " := ");
      field->expression()->Accept(this, data);
    }
    print(")");
  }

  void visitASTCastExpression(const googlesql::ASTCastExpression* node, void* data) override {
    print(node->is_safe_cast() ? "TRY_CAST(" : "CAST(");
    node->expr()->Accept(this, data);
    print("AS");
    node->type()->Accept(this, data);
    print(")");
  }

  void visitASTFunctionCall(const googlesql::ASTFunctionCall* node, void* data) override {
    const googlesql::ASTPathExpression* function = node->function();
    if (node->arguments().empty() && function->num_names() == 1 &&
        IsBareKeywordFunction(ToUpper(function->first_name()->GetAsStringView()))) {
      print(function->first_name()->GetAsStringView());
      return;
    }
    Unparser::visitASTFunctionCall(node, data);
  }

 private:
  // The type name of the column schema currently being unparsed, if any.
  const googlesql::ASTPathExpression* column_type_name_ = nullptr;
  bool column_type_has_parameters_ = false;
};

}  // namespace

std::string TranslateToDuckDbSql(const FrontendResult& frontend_result) {
  std::string unparsed;
  DuckDbUnparser unparser(&unparsed);
  frontend_result.statement().Accept(&unparser, /*data=*/nullptr);
  unparser.FlushLine();
  const size_t end = unparsed.find_last_not_of(" \t\r\n");
  return end == std::string::npos ? std::string() : unparsed.substr(0, end + 1);
}

}  // namespace bigquery_emulator_duckdb
