#include "src/translator/handlers.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "googlesql/public/type.h"
#include "googlesql/public/types/graph_element_type.h"
#include "googlesql/public/types/graph_path_type.h"
#include "nlohmann/json.hpp"
#include "src/duckdb_sql.h"
#include "src/function_catalog/catalog.h"
#include "src/function_catalog/comparison.h"
#include "src/function_catalog/resolved_access.h"
#include "src/function_catalog/rules.h"
#include "src/timestamp_precision.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {

using function_catalog::BindComparison;
using function_catalog::BoundComparison;
using function_catalog::Equal;
using function_catalog::FunctionArgument;
using function_catalog::InArray;
using function_catalog::IsConstant;
using function_catalog::ScalarCall;
using function_catalog::StringLiteral;

namespace {

// The translated arguments of a call.
std::vector<std::string> Sqls(const ScalarCall& call) {
  std::vector<std::string> sqls;
  sqls.reserve(call.arguments.size());
  std::ranges::transform(call.arguments, std::back_inserter(sqls), &FunctionArgument::sql);
  return sqls;
}

}  // namespace

std::optional<std::string> MakeArray(const ScalarCall& call) {
  return "[" + Join(Sqls(call), ", ") + "]";
}

std::optional<std::string> Logical(const ScalarCall& call) {
  if (call.arguments.size() < 2) {
    return std::nullopt;
  }
  return "(" + Join(Sqls(call), call.name == "$AND" ? " AND " : " OR ") + ")";
}

std::optional<std::string> InList(const ScalarCall& call) {
  const std::vector<std::string> args = Sqls(call);
  if (args.size() < 2) {
    return std::nullopt;
  }
  if (call.arguments.front().type == googlesql::TYPE_DOUBLE) {
    return StructComparison(call);
  }
  const std::vector<std::string> values(args.begin() + 1, args.end());
  return "(" + args.at(0) + " IN (" + Join(values, ", ") + "))";
}

std::optional<std::string> Case(const ScalarCall& call) {
  const std::vector<std::string> args = Sqls(call);
  const size_t n = args.size();
  const size_t start = call.name == "$CASE_WITH_VALUE" ? 1 : 0;
  if (n < start + 3 || (n - start) % 2 != 1) {
    return std::nullopt;
  }
  std::string sql = "CASE ";
  if (start == 1) {
    sql += args.at(0) + " ";
  }
  for (size_t i = start; i + 1 < n; i += 2) {
    sql += "WHEN " + args.at(i) + " THEN " + args.at(i + 1) + " ";
  }
  return "(" + sql + "ELSE " + args.back() + " END)";
}

std::optional<std::string> GraphIdentityPredicate(const ScalarCall& call) {
  if (call.safe || call.arguments.size() < 2 ||
      std::ranges::any_of(call.arguments, [](const auto& argument) {
        return argument.type != googlesql::TYPE_GRAPH_ELEMENT;
      })) {
    return std::nullopt;
  }
  std::vector<std::string> identities;
  std::vector<size_t> indexes;
  std::vector<std::string> values;
  std::vector<std::string> nulls;
  for (size_t i = 0; i < call.arguments.size(); ++i) {
    // Compare element identity, including the element table and every key component,
    // without comparing labels or properties. Bind each argument's identity once.
    identities.push_back("struct_extract(" + call.arguments.at(i).sql + ", '$id')");
    indexes.push_back(i);
    values.push_back(BoundComparison(i));
    nulls.push_back(BoundComparison(i) + " IS NULL");
  }
  const auto count = call.name == "SAME" ? 1 : values.size();
  return BindComparison(identities, indexes,
                        "CASE WHEN " + Join(nulls, " OR ") + " THEN " +
                            call.Raise(QuoteLiteral("Input to the function " +
                                                    std::string(call.name) + " must not be null")) +
                            " ELSE list_unique([" + Join(values, ", ") +
                            "]) = " + std::to_string(count) + " END");
}

std::optional<std::string> StructComparison(const ScalarCall& call) {
  const std::vector<std::string> args = Sqls(call);
  const googlesql::Type* type = call.ArgumentType(0);
  const size_t n = args.size();
  if (call.name == "$EQUAL" || call.name == "$NOT_EQUAL") {
    const std::string equal = Equal(type, BoundComparison(0), BoundComparison(1));
    return BindComparison(args, {0, 1}, call.name == "$EQUAL" ? equal : "(NOT " + equal + ")");
  }
  if (call.name == "NULLIF") {
    return BindComparison(args, {0, 1},
                          "CASE WHEN " + Equal(type, BoundComparison(0), BoundComparison(1)) +
                              " THEN NULL ELSE " + BoundComparison(0) + " END");
  }
  if (call.name == "$IN") {
    std::vector<size_t> indexes;
    std::vector<std::string> equals;
    for (size_t i = 0; i < n; ++i) {
      indexes.push_back(i);
      if (i > 0) {
        equals.push_back(Equal(type, BoundComparison(0), BoundComparison(i)));
      }
    }
    return BindComparison(args, indexes, "(" + Join(equals, " OR ") + ")");
  }
  if (call.name == "$IN_ARRAY") {
    return InArray(type, args.at(0), args.at(1));
  }
  if (call.name == "$CASE_WITH_VALUE" && n >= 4 && n % 2 == 0) {
    // The value and the WHEN values are bound to find the matching WHEN, while the results stay
    // in a CASE that evaluates only the one it returns. DuckDB has no subqueries in a lambda.
    std::vector<size_t> indexes = {0};
    std::string match;
    std::string results;
    for (size_t i = 1; i + 1 < n; i += 2) {
      const std::string when = std::to_string(indexes.size());
      indexes.push_back(i);
      match +=
          "WHEN " + Equal(type, BoundComparison(0), BoundComparison(i)) + " THEN " + when + " ";
      results += "WHEN " + when + " THEN " + args.at(i + 1) + " ";
    }
    return "(CASE " + BindComparison(args, indexes, "CASE " + match + "ELSE 0 END") + " " +
           results + "ELSE " + args.back() + " END)";
  }
  return std::nullopt;
}

namespace {

// Original struct names in depth-first order. JSON semantics depend on names, including empty
// and duplicate ones, while DuckDB may hold these fields under positional internal names.
void ValueFieldNames(const googlesql::Type* type, std::vector<std::string>& names) {
  if (type->IsArray()) {
    ValueFieldNames(type->AsArray()->element_type(), names);
  } else if (type->IsStruct()) {
    for (const auto& field : type->AsStruct()->fields()) {
      names.push_back(field.name);
      ValueFieldNames(field.type, names);
    }
  }
}

// Pair each value with its original struct names, without evaluating the value twice. The
// backend reconstructs the GoogleSQL type from these names and the physical value type.
std::optional<std::string> ValueArgument(const ScalarCall& call, size_t i) {
  const auto type = DuckDbType(call.ArgumentType(i), call.sql_timestamp_precision);
  if (!type) {
    return std::nullopt;
  }
  std::vector<std::string> names;
  ValueFieldNames(call.ArgumentType(i), names);
  return "struct_pack(value := CAST(" + call.arguments.at(i).sql + " AS " + *type +
         "), names := " + QuoteLiteral(nlohmann::json(names).dump()) + ")";
}

// The typed value arguments of a call, or nullopt when one has a type DuckDB cannot hold.
std::optional<std::vector<std::string>> ValueArguments(const ScalarCall& call) {
  std::vector<std::string> arguments;
  for (size_t i = 0; i < call.arguments.size(); ++i) {
    const auto argument = ValueArgument(call, i);
    if (!argument) {
      return std::nullopt;
    }
    arguments.push_back(*argument);
  }
  return arguments;
}

}  // namespace

std::optional<std::string> ContainsSubstr(const ScalarCall& call) {
  if (function_catalog::IsNullLiteral(call.Argument(1))) {
    throw std::invalid_argument("The second argument of CONTAINS_SUBSTR() must not be null.");
  }
  const auto constant = function_catalog::IsConstantExpression(call.Argument(1));
  if (!constant) {
    return std::nullopt;
  }
  if (!*constant) {
    throw std::invalid_argument("CONTAINS_SUBSTR search value must be a constant expression");
  }
  const auto value = ValueArgument(call, 0);
  if (!value) {
    return std::nullopt;
  }
  return "bq_contains_substr(" + *value + ", " + call.arguments.at(1).sql + ", " +
         call.arguments.at(2).sql + ")";
}

namespace {

// The JSON text of the graph element, path, or ARRAY of them that `sql` holds, with `flag` as
// stringify_wide_numbers, in the format BigQuery documents for TO_JSON; nullopt for other types,
// such as a STRUCT that holds one. GoogleSQL's TO_JSON formats each element as a STRUCT of the
// object's keys, so properties follow its rules for their types; `depth` keeps nested lambda
// names apart.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): SQL expressions of the value and the flag.
std::optional<std::string> GraphJson(const googlesql::Type* type, const std::string& sql,
                                     const std::string& flag, TimestampPrecision precision,
                                     int depth = 0) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  const auto duckdb_type = DuckDbType(type, precision);
  if (!duckdb_type) {
    return std::nullopt;
  }
  const std::string value = "_gj" + std::to_string(depth);
  const auto bind = [&](const std::string& body) {
    return "list_transform([CAST(" + sql + " AS " + *duckdb_type + ")], lambda " + value +
           ": CASE WHEN " + value + " IS NULL THEN 'null' ELSE " + body + " END)[1]";
  };
  const auto elements = [&](const std::string& count, const std::string& element) {
    const std::string index = "_gi" + std::to_string(depth);
    return "'[' || array_to_string(list_transform(range(" + count + "), lambda " + index + ": " +
           element + "), ',') || ']'";
  };
  if (type->IsArray()) {
    const auto element =
        GraphJson(type->AsArray()->element_type(), value + "[_gi" + std::to_string(depth) + " + 1]",
                  flag, precision, depth + 1);
    return element ? std::optional(bind(elements("len(" + value + ")", *element))) : std::nullopt;
  }
  if (type->IsGraphPath()) {
    const std::string index = "_gi" + std::to_string(depth);
    const std::string offset = "[" + index + " // 2 + 1]";
    const auto node = GraphJson(type->AsGraphPath()->node_type(), value + ".\"$nodes\"" + offset,
                                flag, precision, depth + 1);
    const auto edge = GraphJson(type->AsGraphPath()->edge_type(), value + ".\"$edges\"" + offset,
                                flag, precision, depth + 1);
    if (!node || !edge) {
      return std::nullopt;
    }
    // Nodes and edges alternate, starting and ending with a node.
    return bind(
        elements("len(" + value + ".\"$nodes\") + len(" + value + ".\"$edges\")",
                 "CASE WHEN " + index + " % 2 = 0 THEN " + *node + " ELSE " + *edge + " END"));
  }
  if (!type->IsGraphElement()) {
    return std::nullopt;
  }
  const auto* element = type->AsGraphElement();
  std::vector<std::string> names = {"identifier", "kind", "labels", "properties"};
  for (const auto& property : element->property_types()) {
    names.push_back(property.name);
    ValueFieldNames(property.value_type, names);
  }
  const auto identifier = [&](std::string_view field) {
    return "to_base64(encode(" + value + ".\"" + std::string(field) + "\"))";
  };
  // A node's identifier is its ELEMENT_ID and an edge's endpoints' are its SOURCE_NODE_ID and
  // DESTINATION_NODE_ID, but an edge's own identifier differs from its ELEMENT_ID, as BigQuery
  // answers. Encoding it as a JSON array keeps it apart from every node's.
  const std::string own =
      element->IsEdge() ? "to_base64(encode(CAST(to_json([" + value + ".\"$id\"]) AS VARCHAR)))"
                        : identifier("$id");
  std::string fields =
      "identifier := " + own + ", kind := " + (element->IsEdge() ? "'edge'" : "'node'") +
      ", labels := " + value + ".\"$labels\", properties := " + value + ".\"$properties\"";
  if (element->IsEdge()) {
    names.emplace_back("source_node_identifier");
    names.emplace_back("destination_node_identifier");
    fields += ", source_node_identifier := " + identifier("$source_id") +
              ", destination_node_identifier := " + identifier("$destination_id");
  }
  const std::string json = "bq_to_json(struct_pack(value := struct_pack(" + fields +
                           "), names := " + QuoteLiteral(nlohmann::json(names).dump()) + "), " +
                           flag + ")";
  // Properties the element's table does not define are left out, as BigQuery answers.
  return bind("CASE WHEN len(" + value + ".\"$missing\") = 0 THEN " + json +
              " ELSE list_reduce(list_prepend(" + json + ", list_transform(" + value +
              ".\"$missing\", lambda _gm: '$.properties.\"' || replace(replace(_gm, '\\', "
              "'\\\\'), '\"', '\\\"') || '\"')), lambda _gr, _gp: bq_json_remove(_gr, _gp)) END");
}

}  // namespace

// TO_JSON(value, stringify_wide_numbers) and TO_JSON_STRING(value, pretty_print).
std::optional<std::string> ToJson(const ScalarCall& call) {
  const size_t n = call.arguments.size();
  const std::string flag = n == 2 ? call.arguments.at(1).sql : "false";
  if ((n == 1 || n == 2) && HasGraph(call.ArgumentType(0))) {
    // Only TO_JSON takes graph values.
    if (call.name != "TO_JSON") {
      return std::nullopt;
    }
    const auto json =
        GraphJson(call.ArgumentType(0), "_gf.v", "_gf.f", call.sql_timestamp_precision);
    if (!json) {
      return std::nullopt;
    }
    return "json(list_transform([struct_pack(v := " + call.arguments.at(0).sql + ", f := " + flag +
           ")], lambda _gf: CASE WHEN _gf.f IS NULL THEN NULL ELSE " + *json + " END)[1])";
  }
  const auto value = n == 1 || n == 2 ? ValueArgument(call, 0) : std::nullopt;
  if (!value) {
    return std::nullopt;
  }
  if (call.name == "TO_JSON_STRING") {
    return "bq_to_json_string(" + *value + ", " + flag + ")";
  }
  return "json(bq_to_json(" + *value + ", " + flag + "))";
}

// JSON_REMOVE and JSON_SET take the paths one by one, in order.
std::optional<std::string> JsonRemove(const ScalarCall& call) {
  const std::vector<std::string> args = Sqls(call);
  if (args.size() < 2) {
    return std::nullopt;
  }
  std::string result = "CAST(" + args.at(0) + " AS VARCHAR)";
  for (size_t i = 1; i < args.size(); ++i) {
    result.insert(0, "bq_json_remove(").append(", ").append(args.at(i)).append(")");
  }
  return "json(" + result + ")";
}

std::optional<std::string> JsonSet(const ScalarCall& call) {
  const std::vector<std::string> args = Sqls(call);
  const size_t n = args.size();
  if (n < 4 || n % 2 != 0) {
    return std::nullopt;
  }
  std::string result = "CAST(" + args.at(0) + " AS VARCHAR)";
  for (size_t i = 1; i + 1 < n; i += 2) {
    const auto value = ValueArgument(call, i + 1);
    if (!value) {
      return std::nullopt;
    }
    result.insert(0, "bq_json_set(")
        .append(", ")
        .append(args.at(i))
        .append(", ")
        .append(*value)
        .append(", ")
        .append(args.at(n - 1))
        .append(")");
  }
  return "json(" + result + ")";
}

// JSON_ARRAY_APPEND and JSON_ARRAY_INSERT pass every pair to one backend call so that all
// paths are validated before any mutation. Each argument occurs once, including the flag.
std::optional<std::string> JsonArrayModify(const ScalarCall& call) {
  const size_t n = call.arguments.size();
  if (n < 4 || n % 2 != 0) {
    return std::nullopt;
  }
  std::vector<std::string> args = {"CAST(" + call.arguments.at(0).sql + " AS VARCHAR)"};
  for (size_t i = 1; i + 1 < n; i += 2) {
    const auto value = ValueArgument(call, i + 1);
    if (!value) {
      return std::nullopt;
    }
    args.push_back("CAST(" + call.arguments.at(i).sql + " AS VARCHAR)");
    args.push_back(*value);
  }
  args.push_back("CAST(" + call.arguments.at(n - 1).sql + " AS BOOLEAN)");
  const std::string function =
      call.name == "JSON_ARRAY_INSERT" ? "bq_json_array_insert" : "bq_json_array_append";
  return "json(" + function + "(" + Join(args, ", ") + "))";
}

// JSON_ARRAY(values...).
std::optional<std::string> JsonArray(const ScalarCall& call) {
  const auto args = ValueArguments(call);
  if (!args) {
    return std::nullopt;
  }
  return args->empty() ? "JSON '[]'" : "json(bq_json_array(" + Join(*args, ", ") + "))";
}

// JSON_OBJECT(key, value, ...) and JSON_OBJECT(keys, values).
std::optional<std::string> JsonObject(const ScalarCall& call) {
  const auto args = ValueArguments(call);
  if (!args || args->size() % 2 != 0) {
    return std::nullopt;
  }
  return args->empty() ? "JSON '{}'" : "json(bq_json_object(" + Join(*args, ", ") + "))";
}

std::optional<std::string> ArrayConcat(const ScalarCall& call) {
  const std::vector<std::string> args = Sqls(call);
  const size_t n = args.size();
  if (n == 0) {
    return std::nullopt;
  }
  if (n == 1) {
    return args.at(0);
  }
  // BigQuery returns NULL when any array is NULL, where DuckDB skips it.
  std::vector<std::string> fields;
  std::vector<std::string> nulls;
  std::vector<std::string> lists;
  for (size_t i = 0; i < n; ++i) {
    const std::string field = "a" + std::to_string(i);
    fields.push_back(field + " := " + args.at(i));
    nulls.push_back("_cat." + field + " IS NULL");
    lists.push_back("_cat." + field);
  }
  return "list_transform([struct_pack(" + Join(fields, ", ") + ")], lambda _cat: CASE WHEN " +
         Join(nulls, " OR ") + " THEN NULL ELSE list_concat(" + Join(lists, ", ") + ") END)[1]";
}

// DuckDB's concat() skips NULL, where BigQuery returns NULL; its || operator does not.
std::optional<std::string> ConcatStrings(const ScalarCall& call) {
  if (call.arguments.empty()) {
    return std::nullopt;
  }
  return "(" + Join(Sqls(call), " || ") + ")";
}

// FORMAT goes to GoogleSQL's implementation, which src/backend_functions.cc registers to tell
// the type of each value by its DuckDB type. The values are cast to the DuckDB type that stands
// for their type; values of types DuckDbType does not map are unsupported.
std::optional<std::string> Format(const ScalarCall& call) {
  std::vector<std::string> arguments;
  for (std::size_t i = 0; i < call.arguments.size(); ++i) {
    const auto duckdb_type = DuckDbType(call.ArgumentType(i), call.sql_timestamp_precision);
    if (!duckdb_type) {
      return std::nullopt;
    }
    arguments.push_back("CAST(" + call.arguments.at(i).sql + " AS " + *duckdb_type + ")");
  }
  return "bq_format(" + Join(arguments, ", ") + ")";
}

// GREATEST and LEAST are NULL when any argument is, where DuckDB's skip NULL, and NaN when any
// argument is, which DuckDB orders above every other number and so only GREATEST gets right.
std::optional<std::string> Extremum(const ScalarCall& call) {
  if (call.arguments.empty()) {
    return std::nullopt;
  }
  const bool greatest = call.name == "GREATEST";
  std::string body = "CASE WHEN list_count(_ext) < len(_ext) THEN NULL ";
  if (!greatest && call.ResultType()->IsDouble()) {
    body += "WHEN isnan(list_max(_ext)) THEN list_max(_ext) ";
  }
  body += std::string("ELSE ") + (greatest ? "list_max" : "list_min") + "(_ext) END";
  return "list_transform([[" + Join(Sqls(call), ", ") + "]], lambda _ext: " + body + ")[1]";
}

// KEYS.ADD_KEY_FROM_RAW_BYTES with the key type 'AES_GCM'. 'AES_CBC_PKCS' keys decrypt a
// ciphertext format that no independent source settles, so they stay unsupported.
std::optional<std::string> AddKeyFromRawBytes(const ScalarCall& call) {
  if (call.arguments.size() != 3 || StringLiteral(call.Argument(1)) != "AES_GCM") {
    return std::nullopt;
  }
  return "bq_keys_add_aes_gcm_key(" + call.arguments.at(0).sql + ", " + call.arguments.at(2).sql +
         ")";
}

// KLL extraction requires a constant number of quantiles or phi even though
// GoogleSQL's builtin signatures do not enforce that restriction.
std::optional<std::string> KllExtract(const ScalarCall& call) {
  if (call.arguments.size() != 2) {
    return std::nullopt;
  }
  const auto constant = function_catalog::IsConstantExpression(call.Argument(1));
  if (!constant) {
    return std::nullopt;
  }
  if (!*constant) {
    throw std::invalid_argument("Argument #2 must be constant; error in " + std::string(call.name) +
                                " expression");
  }
  const bool point = call.name == "KLL_QUANTILES.EXTRACT_POINT_INT64" ||
                     call.name == "KLL_QUANTILES.EXTRACT_POINT_FLOAT64";
  const bool integer = call.name == "KLL_QUANTILES.EXTRACT_INT64" ||
                       call.name == "KLL_QUANTILES.EXTRACT_POINT_INT64";
  const std::string function = std::string(point ? "bq_kll_extract_point_" : "bq_kll_extract_") +
                               (integer ? "int64" : "float64");
  return function + "(" + Join(Sqls(call), ", ") + ")";
}

// TEXT_ANALYZE, whose analyzer and analyzer_options BigQuery requires to be constant. GoogleSQL
// passes both, the omitted ones as their defaults.
std::optional<std::string> TextAnalyze(const ScalarCall& call) {
  for (std::size_t i = 1; i < call.arguments.size(); ++i) {
    if (!IsConstant(call.Argument(i))) {
      throw std::invalid_argument("Argument #" + std::to_string(i + 1) +
                                  " must be constant; error in TEXT_ANALYZE expression");
    }
  }
  return "bq_text_analyze(" + Join(Sqls(call), ", ") + ")";
}

// SEARCH, whose search_query and named arguments BigQuery requires to be constant and not NULL.
// The catalog checks that search_query is constant. The backend reads data_to_search as a
// GoogleSQL value, whose NULL STRUCTs, ARRAYs and JSON make their STRING fields NULL.
std::optional<std::string> Search(const ScalarCall& call) {
  constexpr std::array<std::string_view, 5> kNames = {
      "", "", "json_scope", "analyzer", "analyzer_options",
  };
  if (call.arguments.size() != kNames.size()) {
    return std::nullopt;
  }
  for (std::size_t i = 1; i < call.arguments.size(); ++i) {
    const std::string name = "'" + std::string(kNames.at(i)) + "' of SEARCH()";
    if (function_catalog::IsNullLiteral(call.Argument(i))) {
      throw std::invalid_argument(i == 1 ? "The 2nd argument of SEARCH() must not be null"
                                         : name + " must not be null");
    }
    const auto constant = function_catalog::IsConstantExpression(call.Argument(i));
    if (!constant) {
      return std::nullopt;
    }
    if (!*constant) {
      if (i == 1) {
        return std::nullopt;
      }
      throw std::invalid_argument(name + " must be a constant");
    }
  }
  const auto data = ValueArgument(call, 0);
  if (!data) {
    return std::nullopt;
  }
  std::vector<std::string> arguments = Sqls(call);
  arguments.front() = *data;
  return "bq_search(" + Join(arguments, ", ") + ")";
}

// OBJ.MAKE_REF from a URI, with the authorizer, version and details GoogleSQL passes as NULL when
// omitted, or from an ObjectRef whose authorizer it replaces. The backend checks the authorizer,
// qualifying one with the query's project from a URI, and the details. A NULL URI or ObjectRef
// makes NULL. The JSON form is unsupported.
std::optional<std::string> MakeRef(const ScalarCall& call) {
  const auto type = DuckDbType(call.ResultType(), call.sql_timestamp_precision);
  const std::vector<std::string> args = Sqls(call);
  if (!type || (args.size() != 4 && args.size() != 2)) {
    return std::nullopt;
  }
  const std::string project = QuoteLiteral(call.project);
  if (args.size() == 2) {
    return "list_transform([" + args.at(0) +
           "], lambda _objref: CASE WHEN _objref IS NULL THEN NULL ELSE CAST(struct_pack(uri := "
           "_objref.uri, version := _objref.version, authorizer := bq_obj_replace_authorizer(" +
           args.at(1) + ", " + project + "), details := _objref.details) AS " + *type + ") END)[1]";
  }
  return "list_transform([" + args.at(0) +
         "], lambda _objref: CASE WHEN _objref IS NULL THEN NULL ELSE CAST(struct_pack(uri := "
         "_objref, "
         "version := " +
         args.at(2) + ", authorizer := bq_obj_authorizer(" + args.at(1) + ", " + project +
         "), details := bq_obj_details(" + args.at(3) + ")) AS " + *type + ") END)[1]";
}

// OBJ.FETCH_METADATA of an ObjectRef or of each in an array, which the backend reads as JSON in
// the query's project.
std::optional<std::string> FetchMetadata(const ScalarCall& call) {
  const googlesql::Type* result = call.ResultType();
  const auto type = DuckDbType(result->IsArray() ? result->AsArray()->element_type() : result,
                               call.sql_timestamp_precision);
  if (!type || call.arguments.size() != 1) {
    return std::nullopt;
  }
  const auto fetch = [&](const std::string& objectref) {
    return "CAST(bq_obj_fetch_metadata(to_json(" + objectref + "), " + QuoteLiteral(call.project) +
           ") AS " + *type + ")";
  };
  if (result->IsArray()) {
    return "list_transform(" + call.arguments.at(0).sql + ", lambda _objref: " + fetch("_objref") +
           ")";
  }
  return fetch(call.arguments.at(0).sql);
}

}  // namespace bigquery_emulator_duckdb::translator
