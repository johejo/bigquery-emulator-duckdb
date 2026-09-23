#include "src/translator.h"

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/types/span.h"
#include "googlesql/parser/parse_tree.h"
#include "googlesql/parser/unparser.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/type.h"
#include "src/duckdb_sql.h"
#include "src/functions.h"
#include "src/query_parameters.h"

namespace bigquery_emulator_duckdb {
namespace {

// The unparser formats as it prints, so its output carries the surrounding line breaks.
std::string Trim(std::string_view text) {
  const size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) {
    return {};
  }
  return std::string(text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1));
}

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

  // SELECT * EXCEPT (a) -> SELECT * EXCLUDE (a). REPLACE is spelled the same in DuckDB.
  void visitASTStarModifiers(const googlesql::ASTStarModifiers* node, void* data) override {
    if (node->except_list() != nullptr) {
      print("EXCLUDE (");
      node->except_list()->Accept(this, data);
      print(")");
    }
    if (!node->replace_items().empty()) {
      print("REPLACE (");
      UnparseVectorWithSeparator(node->replace_items(), data, ",");
      print(")");
    }
  }

  // A rewritten call is a new expression rather than a call, so the OVER clause the base class
  // prints after it would no longer attach to a function. The call that the OVER belongs to
  // keeps its BigQuery spelling; the plain renames below still apply to it. The flag reaches
  // exactly that call, which is the first one the base class visits, and is cleared there.
  void visitASTAnalyticFunctionCall(const googlesql::ASTAnalyticFunctionCall* node,
                                    void* data) override {
    in_analytic_function_ = true;
    Unparser::visitASTAnalyticFunctionCall(node, data);
    in_analytic_function_ = false;
  }

  void visitASTFunctionCall(const googlesql::ASTFunctionCall* node, void* data) override {
    const googlesql::ASTPathExpression* function = node->function();
    // SAFE.f(x) returns NULL where f(x) would fail. DuckDB has no such call, so the prefix is
    // dropped and the error reaches the client instead of becoming a NULL.
    const bool is_safe = function->num_names() == 2 &&
                         ToUpperAscii(function->first_name()->GetAsStringView()) == "SAFE" &&
                         !node->is_chained_call();
    if (node->is_chained_call() || (function->num_names() != 1 && !is_safe)) {
      Unparser::visitASTFunctionCall(node, data);
      return;
    }

    const bool is_analytic = std::exchange(in_analytic_function_, false);
    const std::string_view name = function->name(is_safe ? 1 : 0)->GetAsStringView();
    const std::string upper_name = ToUpperAscii(name);
    const absl::Span<const googlesql::ASTExpression* const> arguments = node->arguments();
    if (arguments.empty() && IsBareKeywordFunction(upper_name)) {
      print(name);
      return;
    }

    if (!node->HasModifiers() && !is_analytic) {
      const std::optional<std::string_view> rewrite =
          DuckDbFunctionTemplate(upper_name, arguments.size());
      if (rewrite.has_value()) {
        print(ExpandTemplate(*rewrite, arguments));
        return;
      }
    }

    const std::optional<std::string_view> renamed = DuckDbFunctionName(upper_name);
    if (!renamed.has_value() && !is_safe) {
      Unparser::visitASTFunctionCall(node, data);
      return;
    }
    // Only the name is replaced, so DISTINCT, IGNORE NULLS and the other modifiers the base
    // class prints after it keep working.
    print(renamed.has_value() ? *renamed : name);
    UnparseFunctionArguments(node, arguments, data);
  }

 private:
  // Unparses `node` into a string of its own, so that it can be placed into a rewrite template.
  std::string UnparseToString(const googlesql::ASTNode* node) {
    std::string unparsed;
    DuckDbUnparser unparser(&unparsed, parameters_);
    node->Accept(&unparser, /*data=*/nullptr);
    unparser.FlushLine();
    return Trim(unparsed);
  }

  // Fills in a DuckDbFunctionTemplate: $n is the n-th argument and #n the n-th argument as the
  // lower case string literal DuckDB expects a date part to be.
  std::string ExpandTemplate(std::string_view spelling,
                             absl::Span<const googlesql::ASTExpression* const> arguments) {
    std::string result;
    for (size_t i = 0; i < spelling.size(); ++i) {
      const bool is_placeholder = (spelling[i] == '$' || spelling[i] == '#') &&
                                  i + 1 < spelling.size() &&
                                  std::isdigit(static_cast<unsigned char>(spelling[i + 1])) != 0;
      if (!is_placeholder) {
        result += spelling[i];
        continue;
      }
      const size_t index = static_cast<size_t>(spelling[i + 1] - '1');
      const std::string argument = UnparseToString(arguments[index]);
      result += spelling[i] == '$' ? argument : QuoteLiteral(ToLowerAscii(argument));
      ++i;
    }
    return result;
  }

  // The type name of the column schema currently being unparsed, if any.
  const googlesql::ASTPathExpression* column_type_name_ = nullptr;
  bool column_type_has_parameters_ = false;
  // Set while the base class walks into the function of an analytic call, so that the call
  // the OVER clause belongs to can be told apart from any call nested inside the window spec.
  bool in_analytic_function_ = false;
  const QueryParameters& parameters_;
};

}  // namespace

std::string TranslateToDuckDbSql(const FrontendResult& frontend_result,
                                 const QueryParameters& parameters) {
  std::string unparsed;
  DuckDbUnparser unparser(&unparsed, parameters);
  frontend_result.statement().Accept(&unparser, /*data=*/nullptr);
  unparser.FlushLine();
  return Trim(unparsed);
}

}  // namespace bigquery_emulator_duckdb
