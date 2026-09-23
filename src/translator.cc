#include "src/translator.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "googlesql/parser/parse_tree.h"
#include "googlesql/parser/unparser.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/type.h"
#include "src/duckdb_sql.h"
#include "src/query_parameters.h"

namespace bigquery_emulator_duckdb {
namespace {

// Functions that GoogleSQL spells with empty parentheses and DuckDB spells as a bare keyword.
bool IsBareKeywordFunction(std::string_view upper_name) {
  return upper_name == "CURRENT_TIMESTAMP" || upper_name == "CURRENT_DATE" ||
         upper_name == "CURRENT_TIME";
}

// Unparses a GoogleSQL AST as DuckDB SQL: the base class knows how to print GoogleSQL, and the
// overrides below replace the constructs whose spelling differs in DuckDB.
class DuckDbUnparser : public googlesql::parser::Unparser {
 public:
  DuckDbUnparser(std::string* unparsed, const QueryParameters& parameters)
      : Unparser(unparsed), parameters_(parameters) {}

  void visitASTIdentifier(const googlesql::ASTIdentifier* node, void* data) override {
    if (node->is_quoted()) {
      print(QuoteIdentifierPath(node->GetAsStringView()));
    } else {
      print(node->GetAsStringView());
    }
  }

  // GoogleSQL literals are already unescaped by the parser, so the value only has to be quoted
  // the way DuckDB expects. This covers "...", '''...''' and the r prefix in one place.
  void visitASTStringLiteral(const googlesql::ASTStringLiteral* node, void* data) override {
    print(QuoteLiteral(node->string_value()));
  }

  // Bytes may contain quotes, NUL and non-UTF-8 sequences, none of which survive a string
  // literal, so they go through their hex encoding.
  void visitASTBytesLiteral(const googlesql::ASTBytesLiteral* node, void* data) override {
    print("from_hex(" + QuoteLiteral(ToHex(node->bytes_value())) + ")");
  }

  // GoogleSQL treats every literal with a decimal point as FLOAT64, whereas DuckDB infers a
  // DECIMAL, so the type is pinned down explicitly.
  void visitASTFloatLiteral(const googlesql::ASTFloatLiteral* node, void* data) override {
    print(std::string(node->image()) + "::DOUBLE");
  }

  void visitASTSimpleType(const googlesql::ASTSimpleType* node, void* data) override {
    print(DuckDbTypeName(node->type_name()->ToIdentifierPathString(),
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
      print(DuckDbTypeName(node->ToIdentifierPathString(), column_type_has_parameters_));
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
    print(DuckDbTypeName(
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

  // @name and ? are replaced by the literal of the matching request parameter. DuckDB has
  // prepared statement parameters of its own, but a literal keeps the whole statement in one
  // string and makes ARRAY and STRUCT parameters fall out of the same spelling rules.
  void visitASTParameterExpr(const googlesql::ASTParameterExpr* node, void* data) override {
    print(node->name() == nullptr
              ? parameters_.ByPosition(node->position())
              : parameters_.ByName(std::string(node->name()->GetAsStringView())));
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
        IsBareKeywordFunction(ToUpperAscii(function->first_name()->GetAsStringView()))) {
      print(function->first_name()->GetAsStringView());
      return;
    }
    Unparser::visitASTFunctionCall(node, data);
  }

 private:
  // The type name of the column schema currently being unparsed, if any.
  const googlesql::ASTPathExpression* column_type_name_ = nullptr;
  bool column_type_has_parameters_ = false;
  const QueryParameters& parameters_;
};

}  // namespace

std::string TranslateToDuckDbSql(const FrontendResult& frontend_result,
                                 const QueryParameters& parameters) {
  std::string unparsed;
  DuckDbUnparser unparser(&unparsed, parameters);
  frontend_result.statement().Accept(&unparser, /*data=*/nullptr);
  unparser.FlushLine();
  const size_t end = unparsed.find_last_not_of(" \t\r\n");
  return end == std::string::npos ? std::string() : unparsed.substr(0, end + 1);
}

}  // namespace bigquery_emulator_duckdb
