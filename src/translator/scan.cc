#include "src/translator/scan.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/time/time.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/function.h"
#include "googlesql/public/sql_tvf.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/analyzer_functions/graph_expand.h"
#include "src/catalog_table.h"
#include "src/connection_id.h"
#include "src/duckdb_sql.h"
#include "src/external_query.h"
#include "src/function_catalog/catalog.h"
#include "src/statement_resources.h"
#include "src/table_metadata.h"
#include "src/table_source.h"
#include "src/timestamp_precision.h"
#include "src/translator/aggregate.h"
#include "src/translator/collation.h"
#include "src/translator/context.h"
#include "src/translator/differential_privacy.h"
#include "src/translator/expression.h"
#include "src/translator/function.h"
#include "src/translator/gap_fill.h"
#include "src/translator/graph.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

constexpr auto kZoneIndependentFunctions = std::to_array<std::string_view>({
    "current_timestamp",
    "timestamp_add",
    "timestamp_sub",
});

// Converts an external result column to the BigQuery type the catalog gave it. MySQL decimals
// and PostgreSQL numerics beyond DuckDB's precision arrive as text. MySQL TIMESTAMP arrives as
// a UTC timestamp without a time zone. Dates and times outside BigQuery's ranges fail.
std::string ExternalValue(const googlesql::Type* type, const std::string& value) {
  std::string sql = "CAST(" + value + " AS " +
                    DuckDbType(type, TimestampPrecision::kMicroseconds).value_or("VARCHAR") + ")";
  const auto in_range = [&](const std::string& condition, std::string_view name) {
    return "CASE WHEN " + sql + " IS NULL OR " + condition + " THEN " + sql + " ELSE error('" +
           std::string(name) + " value out of range: ' || CAST(" + sql + " AS VARCHAR)) END";
  };
  switch (type->kind()) {
    case googlesql::TYPE_BIGNUMERIC:
      return "CAST(bq_bignumeric_from_string(CAST(" + value + " AS VARCHAR), false) AS BIGNUM)";
    case googlesql::TYPE_DATE:
      return in_range(sql + " BETWEEN DATE '0001-01-01' AND DATE '9999-12-31'", "DATE");
    case googlesql::TYPE_DATETIME:
      return in_range(sql +
                          " BETWEEN TIMESTAMP '0001-01-01 00:00:00' AND TIMESTAMP '9999-12-31 "
                          "23:59:59.999999'",
                      "DATETIME");
    case googlesql::TYPE_TIMESTAMP:
      return in_range(sql +
                          " BETWEEN TIMESTAMPTZ '0001-01-01 00:00:00+00' AND TIMESTAMPTZ "
                          "'9999-12-31 23:59:59.999999+00'",
                      "TIMESTAMP");
    case googlesql::TYPE_TIME:
      return in_range(sql + " < TIME '24:00:00'", "TIME");
    default:
      return sql;
  }
}

// EXTERNAL_QUERY reads from the database that the catalog attached when it analyzed the query.
std::optional<Relation> ExternalQueryScan(const googlesql::ResolvedTVFScan& tvf,
                                          const Scope& scope) {
  const auto& arguments = tvf.signature()->input_arguments();
  const auto literal = [&](size_t i) {
    return arguments.at(i).GetScalarArgType()->literal_value()->string_value();
  };
  // ExternalValue reads TIMESTAMPs as microseconds.
  if (scope.context.timestamp_precision == TimestampPrecision::kNanoseconds) {
    return Unsupported(scope, "EXTERNAL_QUERY with nanosecond TIMESTAMPs");
  }
  // The catalog accepted the connection ID.
  const std::optional<std::string> key = ConnectionKey(literal(0), scope.context.defaults.project);
  if (!key) {
    return Unsupported(scope, "EXTERNAL_QUERY connection ID");
  }
  std::vector<std::string> names;
  names.reserve(static_cast<size_t>(tvf.signature()->result_schema().num_columns()));
  for (int i = 0; i < tvf.signature()->result_schema().num_columns(); ++i) {
    names.push_back(QuoteIdentifier("_x" + std::to_string(i)));
  }
  Relation result;
  std::vector<std::string> projections;
  for (int i = 0; i < tvf.column_list_size(); ++i) {
    const auto& column = tvf.column_list(i);
    const std::string alias = ColumnName(column.column_id());
    projections.push_back(ExternalValue(column.type(), "e." + names.at(static_cast<size_t>(
                                                                  tvf.column_index_list(i)))) +
                          " AS " + alias);
    result.columns.emplace(column.column_id(), "q." + alias);
  }
  result.sql = "SELECT " + (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) +
               " FROM memory.main." + QuoteIdentifier(ExternalDatabaseName(*key)) + "(" +
               QuoteLiteral(literal(1)) + ") AS e(" + Join(names, ", ") + ")";
  return result;
}

std::optional<std::string> GraphExpandValue(const analyzer_functions::GraphExpandColumn& column,
                                            const googlesql::Type* type,
                                            const std::vector<std::string>& values) {
  const auto& value = values.at(column.body_index);
  if (!column.is_row) {
    return value;
  }
  const auto* record = type->AsStruct();
  if (record == nullptr) {
    return std::nullopt;
  }
  const auto keys = DuckDbStructFieldNames(record);
  std::vector<std::string> fields;
  for (int i = 0; i < record->num_fields(); ++i) {
    const auto& field = record->field(i);
    const auto source =
        std::ranges::find(column.fields, field.name, &analyzer_functions::GraphExpandColumn::name);
    if (source == column.fields.end()) {
      return std::nullopt;
    }
    const auto field_value = GraphExpandValue(*source, field.type, values);
    if (!field_value) {
      return std::nullopt;
    }
    fields.push_back(QuoteIdentifier(keys.at(i)) + " := " + *field_value);
  }
  if (fields.empty()) {
    fields.push_back(QuoteIdentifier(kEmptyStruct) + " := TRUE");
  }
  return "CASE WHEN " + value + " IS NOT NULL THEN struct_pack(" + Join(fields, ", ") + ") END";
}

std::optional<Relation> GraphExpandScan(const googlesql::ResolvedTVFScan& tvf,
                                        const analyzer_functions::GraphExpandBody& body,
                                        const Scope& scope) {
  const auto input = Scan(*body.query.query(), Scope{.context = scope.context});
  if (!input) {
    return std::nullopt;
  }
  std::vector<std::string> values;
  std::ranges::transform(
      body.query.output_column_list(), std::back_inserter(values),
      [&](const auto& output) { return input->columns.at(output->column().column_id()); });
  Relation result;
  std::vector<std::string> projections;
  for (int i = 0; i < tvf.column_list_size(); ++i) {
    const auto& column = tvf.column_list(i);
    const auto& source = body.columns.at(tvf.column_index_list(i));
    const auto value = GraphExpandValue(source, column.type(), values);
    if (!value) {
      return Unsupported(scope, "GRAPH_EXPAND ROW projection");
    }
    const std::string alias = ColumnName(column.column_id());
    projections.push_back(*value + " AS " + alias);
    result.columns.emplace(column.column_id(), "q." + alias);
  }
  result.sql =
      "SELECT " + (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) + input->From();
  return result;
}

// A relation argument of VECTOR_SEARCH as its rows (`row`) and the embeddings it searches with
// (`embedding`), which `embedding` gives from the columns' values, or nullopt where its columns
// cannot be read.
std::optional<std::string> SearchedRows(
    const googlesql::ResolvedFunctionArgument& argument, const googlesql::TVFRelation& schema,
    const googlesql::Type* type, const Scope& scope,
    const std::function<std::string(const std::vector<std::string>&)>& embedding) {
  const auto input = Scan(*argument.scan(), scope);
  const auto sql_type = DuckDbType(type, scope.context.timestamp_precision);
  if (!input || !sql_type) {
    return std::nullopt;
  }
  const auto names = DuckDbStructFieldNames(type->AsStruct());
  std::vector<std::string> fields;
  std::vector<std::string> values;
  for (int i = 0; i < schema.num_columns(); ++i) {
    const auto value = input->columns.find(argument.argument_column_list(i).column_id());
    if (value == input->columns.end()) {
      return std::nullopt;
    }
    fields.push_back(QuoteIdentifier(names.at(static_cast<size_t>(i))) + " := " + value->second);
    values.push_back(value->second);
  }
  return "SELECT CAST(struct_pack(" + Join(fields, ", ") + ") AS " + *sql_type + ") AS row, " +
         embedding(values) + " AS embedding, row_number() OVER () AS id" + input->From();
}

bool IsEmbedding(const googlesql::Type* type) {
  return type->IsArray() && type->AsArray()->element_type()->IsDouble();
}

// A searched relation's column names, which BigQuery gives a value table as $col1.
std::vector<std::string> SearchedNames(const googlesql::TVFRelation& schema) {
  if (schema.is_value_table()) {
    return {"$col1"};
  }
  std::vector<std::string> names;
  names.reserve(static_cast<size_t>(schema.num_columns()));
  for (int i = 0; i < schema.num_columns(); ++i) {
    names.push_back(schema.column(i).name);
  }
  return names;
}

// A CASE on the column name `name`, SQL that is a literal or a query parameter, giving `value(i)`
// for the column i it selects, ignoring case, as the catalog does; `otherwise` where it selects
// none or several.
std::string SelectColumn(const std::string& name, const std::vector<std::string>& names,
                         const std::function<std::string(int)>& value,
                         const std::string& otherwise) {
  std::map<std::string, int> counts;
  for (const auto& each : names) {
    ++counts[absl::AsciiStrToLower(each)];
  }
  std::string result = "CASE lower(" + name + ")";
  for (std::size_t i = 0; i < names.size(); ++i) {
    const std::string lower = absl::AsciiStrToLower(names.at(i));
    if (counts.at(lower) == 1) {
      result += " WHEN " + QuoteLiteral(lower) + " THEN " + value(static_cast<int>(i));
    }
  }
  return result + " ELSE " + otherwise + " END";
}

std::string SqlError(const std::string& message) { return "error(" + message + ")"; }

// The distance of type `type` between `e.b` and `e.q` as `raw`, which the pairs compute as
// `r.d`, and `checked`, which reads it with BigQuery's errors. NaN elements give NaN distances,
// which BigQuery orders first, even where terms overflow, and infinite elements infinite or NaN
// ones. Otherwise an infinite term or sum fails.
struct SearchDistance {
  std::string raw;
  std::string checked;
};

SearchDistance VectorSearchDistance(const std::string& type) {
  std::string raw;
  std::string term;
  std::string zero;
  if (type == "EUCLIDEAN") {
    raw = "list_distance(e.b, e.q)";
    term = "(e.b[i] - e.q[i]) * (e.b[i] - e.q[i])";
  } else if (type == "COSINE") {
    // BigQuery's cosine distance divides by the square root of the norms' product, as its
    // examples' last digits show, rather than by the product of their square roots, and only
    // the dot product fails to overflow: overflowing norms make the distance 1.
    raw =
        "1 - list_inner_product(e.b, e.q) / sqrt(list_inner_product(e.b, e.b) * "
        "list_inner_product(e.q, e.q))";
    term = "e.b[i] * e.q[i]";
    zero =
        "WHEN list_inner_product(e.b, e.b) = 0 OR list_inner_product(e.q, e.q) = 0 THEN "
        "error('Cannot compute cosine distance against zero vector.') ";
  } else {
    // BigQuery's DOT_PRODUCT distance is the negated dot product, so the largest is nearest.
    raw = "-list_inner_product(e.b, e.q)";
    term = "e.b[i] * e.q[i]";
  }
  const std::string sum = type == "EUCLIDEAN" ? "r.d" : "list_inner_product(e.b, e.q)";
  return {
      .raw = raw,
      .checked = std::format(
          "CASE {}WHEN r.inf OR r.nan THEN r.d WHEN isinf({}) OR len(list_filter(list_transform("
          "range(1, len(e.b) + 1), lambda i: {}), lambda t: isinf(t))) > 0 THEN error('Compute "
          "overflow') ELSE r.d END",
          zero, sum, term),
  };
}

// VECTOR_SEARCH's brute-force search: the top_k base rows nearest each query embedding. The
// catalog validated the literal arguments; top_k and distance_type can also be query
// parameters. The errors are BigQuery's, observed in
// tests/e2e/goclient/testdata/scalars/vector_search.txt.
std::optional<Relation> VectorSearchScan(const googlesql::ResolvedTVFScan& tvf,
                                         const Scope& scope) {
  const auto& signature = *tvf.function_call_signature();
  const auto& arguments = tvf.signature()->input_arguments();
  const auto index = [&](std::string_view name) {
    for (int i = 0; i < signature.NumConcreteArguments(); ++i) {
      const auto& argument = signature.ConcreteArgument(i);
      if (argument.has_argument_name() && argument.argument_name() == name) {
        return i;
      }
    }
    throw std::logic_error("VECTOR_SEARCH has no argument " + std::string(name));
  };
  // The literal value of an argument, or nullopt for a query parameter.
  const auto literal = [&](std::string_view name) -> std::optional<googlesql::Value> {
    const auto scalar = arguments.at(static_cast<size_t>(index(name))).GetScalarArgType();
    if (!scalar.ok() || scalar->literal_value() == nullptr) {
      return std::nullopt;
    }
    return *scalar->literal_value();
  };
  // A query parameter's SQL.
  const auto parameter = [&](std::string_view name) -> std::optional<std::string> {
    const int i = index(name);
    if (i >= tvf.argument_list_size() || tvf.argument_list(i)->expr() == nullptr) {
      return std::nullopt;
    }
    return Expression(*tvf.argument_list(i)->expr(), scope, Columns{});
  };
  // An argument's SQL: its literal or the query parameter the catalog let through.
  const auto argument_sql = [&](std::string_view name) -> std::optional<std::string> {
    if (const auto value = literal(name)) {
      return value->is_null() ? "NULL" : QuoteLiteral(value->string_value());
    }
    return parameter(name);
  };
  const auto& result = tvf.signature()->result_schema();
  const bool batch = tvf.argument_list(2)->scan() != nullptr;
  const auto& base_schema = arguments.at(0).relation();
  const auto base_names = SearchedNames(base_schema);
  const auto base_type = [&](int i) { return base_schema.column(i).type; };
  const auto column = argument_sql("column_to_search");
  if (!column) {
    return std::nullopt;
  }
  // The checks of arguments that are query parameters, which the catalog made for literals with
  // the same errors.
  std::vector<std::string> checks;
  checks.push_back(
      "CASE WHEN " + *column + " IS NULL THEN " +
      "error('2nd argument of VECTOR_SEARCH must be specified.') ELSE " +
      SelectColumn(
          *column, base_names,
          [&](int i) {
            const std::string& name = base_names.at(static_cast<size_t>(i));
            if (IsEmbedding(base_type(i))) {
              return std::string("TRUE");
            }
            if (base_type(i)->IsString() || base_type(i)->IsStruct()) {
              return SqlError(QuoteLiteral(
                  "No generation expression found for the column_to_search: " + name + "."));
            }
            return SqlError(QuoteLiteral(
                "column_to_search argument of VECTOR_SEARCH: " + name +
                " should be of type ARRAY<FLOAT64>, STRING, ObjectRef, or STRUCT<ARRAY<FLOAT64>, "
                "STRING>."));
          },
          SqlError("'Column ' || " + *column +
                   " || ' should exist in the 1st argument of VECTOR_SEARCH.'")) +
      " END");
  const auto base_name = SelectColumn(
      *column, base_names,
      [&](int i) { return QuoteLiteral(base_names.at(static_cast<size_t>(i))); }, "NULL");
  const auto base =
      SearchedRows(*tvf.argument_list(0), base_schema, result.column(batch ? 1 : 0).type, scope,
                   [&](const std::vector<std::string>& values) {
                     return SelectColumn(
                         *column, base_names,
                         [&](int i) {
                           return IsEmbedding(base_type(i)) ? values.at(static_cast<size_t>(i))
                                                            : std::string("NULL");
                         },
                         "NULL");
                   });
  if (!base) {
    return std::nullopt;
  }
  // The query value is bound once, as the batch form's query rows are.
  std::string query;
  std::string mismatch = QuoteLiteral("Array inputs are not equal in length");
  if (batch) {
    const auto& query_schema = arguments.at(2).relation();
    const auto query_names = SearchedNames(query_schema);
    const auto query_type = [&](int i) { return query_schema.column(i).type; };
    const auto named = argument_sql("query_column_to_search");
    if (!named) {
      return std::nullopt;
    }
    std::vector<int> embeddings;
    for (int i = 0; i < query_schema.num_columns(); ++i) {
      if (IsEmbedding(query_type(i))) {
        embeddings.push_back(i);
      }
    }
    // The query column: the one named, or without a name, the ARRAY<FLOAT64> column named like
    // the base column, or else the only ARRAY<FLOAT64> column; as CASEs giving `value(i)` for it,
    // and checking it. A NULL parameter, as a NULL literal, names none.
    const bool one = embeddings.size() == 1;
    const std::string none = SqlError(
        "'One and only one column of ARRAY<FLOAT64> type should exist in 3rd argument of "
        "VECTOR_SEARCH when query_column_to_search is not specified and no ARRAY<FLOAT64> column "
        "with the same name as ' || " +
        *column + " || ' exists in the 3rd argument.'");
    const auto default_column = [&](const std::function<std::string(int)>& value) {
      const std::string only = one ? value(embeddings.front()) : "NULL";
      return SelectColumn(
          *column, query_names, [&](int i) { return IsEmbedding(query_type(i)) ? value(i) : only; },
          only);
    };
    const std::string default_check = SelectColumn(
        *column, query_names,
        [&](int i) { return IsEmbedding(query_type(i)) || one ? std::string("TRUE") : none; },
        one ? "TRUE" : none);
    const auto named_column = [&](const std::function<std::string(int)>& value) {
      return SelectColumn(*named, query_names, value, "NULL");
    };
    const std::string named_check = SelectColumn(
        *named, query_names,
        [&](int i) {
          return IsEmbedding(query_type(i))
                     ? std::string("TRUE")
                     : SqlError(QuoteLiteral("query_column_to_search argument of VECTOR_SEARCH: " +
                                             query_names.at(static_cast<size_t>(i)) +
                                             " should be of type ARRAY<FLOAT64>."));
        },
        SqlError(*named + " || ' should exist in 3rd argument of VECTOR_SEARCH.'"));
    const auto query_column = [&](const std::function<std::string(int)>& value) {
      return "CASE WHEN " + *named + " IS NULL THEN " + default_column(value) + " ELSE " +
             named_column(value) + " END";
    };
    checks.push_back("CASE WHEN " + *named + " IS NULL THEN " + default_check + " ELSE " +
                     named_check + " END");
    const auto rows = SearchedRows(*tvf.argument_list(2), query_schema, result.column(0).type,
                                   scope, [&](const std::vector<std::string>& values) {
                                     return query_column([&](int i) {
                                       return IsEmbedding(query_type(i))
                                                  ? values.at(static_cast<size_t>(i))
                                                  : std::string("NULL");
                                     });
                                   });
    if (!rows) {
      return std::nullopt;
    }
    query = *rows;
    mismatch =
        "'Dimension of column `' || " + base_name +
        " || '` in the base table does not match the dimension of column `' || " +
        query_column([&](int i) { return QuoteLiteral(query_names.at(static_cast<size_t>(i))); }) +
        " || '` in the query data.'";
  } else {
    const auto value = Expression(*tvf.argument_list(2)->expr(), scope, Columns{});
    if (!value) {
      return std::nullopt;
    }
    query = "SELECT NULL AS row, " + *value + " AS embedding, NULL AS id";
  }
  if (!literal("options")) {
    const auto options = parameter("options");
    if (!options) {
      return std::nullopt;
    }
    // BigQuery ignores a NULL parameter, as the default {}.
    checks.push_back("CASE WHEN " + *options + " IS NULL THEN TRUE ELSE bq_vector_search_options(" +
                     *options + ") END");
  }
  SearchDistance distance;
  if (const auto type = literal("distance_type")) {
    // A NULL literal is the default.
    distance = VectorSearchDistance(type->is_null() ? "EUCLIDEAN"
                                                    : absl::AsciiStrToUpper(type->string_value()));
  } else {
    // A parameter selects the distance type as its value names it, ignoring case, and a NULL
    // one the default.
    const auto parameter_value = parameter("distance_type");
    if (!parameter_value) {
      return std::nullopt;
    }
    const std::string name = "COALESCE(" + *parameter_value + ", 'EUCLIDEAN')";
    std::string raw = "CASE upper(" + name + ")";
    std::string checked = raw;
    for (const std::string each_type : {"EUCLIDEAN", "COSINE", "DOT_PRODUCT"}) {
      const auto each = VectorSearchDistance(each_type);
      raw += " WHEN '" + each_type + "' THEN " + each.raw;
      checked += " WHEN '" + each_type + "' THEN " + each.checked;
    }
    distance = {
        .raw = raw + " END",
        .checked = checked +
                   " ELSE error('distance_type argument of VECTOR_SEARCH should be one of: "
                   "EUCLIDEAN, COSINE, DOT_PRODUCT. Got ' || " +
                   name + " || '.') END",
    };
  }
  // BigQuery searches NULL embeddings as empty ones, whose dimension differs from the others',
  // except that a NULL single query value that is not a literal gives NULL distances. The
  // distance's CASE evaluates the checks first, since DuckDB can evaluate the pairs before the
  // checks' join below, and a pair's error would then replace the checks' error.
  const std::string ok = "(" + Join(checks, " AND ") + ")";
  const std::string pairs = std::format(
      "SELECT vq.row AS query, vq.id AS query_id, vb.row AS base, d.distance FROM ({}) AS vq "
      "CROSS JOIN ({}) AS vb, LATERAL (SELECT "
      "COALESCE(vb.embedding, []::DOUBLE[]) AS b, "
      "COALESCE(vq.embedding, []::DOUBLE[]) AS q) AS e, LATERAL (SELECT len(list_filter(e.q, "
      "lambda x: x IS NULL)) > 0 AS null_query, len(e.b) <> len(e.q) AS mismatch) AS c, LATERAL "
      "(SELECT CASE WHEN NOT c.null_query AND NOT c.mismatch THEN {} END AS d, "
      "len(list_filter(e.b || e.q, lambda x: isinf(x))) > 0 AS inf, len(list_filter(e.b || e.q, "
      "lambda x: isnan(x))) > 0 AS nan) AS r, LATERAL (SELECT CASE WHEN NOT {} THEN NULL {}WHEN "
      "c.null_query THEN "
      "error('Unexpected NULL element in input array') WHEN c.mismatch THEN error({}) ELSE {} END "
      "AS distance) AS d",
      query, *base, distance.raw, ok, batch ? "" : "WHEN vq.embedding IS NULL THEN NULL ", mismatch,
      distance.checked);
  // A NULL top_k is the default, 10, and a negative one returns every base row.
  std::string top_k;
  if (const auto value = literal("top_k")) {
    top_k = value->is_null() ? "10" : std::to_string(value->int64_value());
  } else {
    const auto k = parameter("top_k");
    if (!k) {
      return std::nullopt;
    }
    top_k = "COALESCE(" + *k + ", 10)";
  }
  const std::string ranked =
      "SELECT *, row_number() OVER (PARTITION BY query_id ORDER BY isnan(distance) DESC, "
      "distance) AS rank FROM (" +
      pairs + ")";
  Relation output;
  std::vector<std::string> projections;
  for (int i = 0; i < tvf.column_list_size(); ++i) {
    const int id = tvf.column_list(i).column_id();
    const std::string& name = result.column(tvf.column_index_list(i)).name;
    projections.push_back("vs." + QuoteIdentifier(name) + " AS " + ColumnName(id));
    output.columns.emplace(id, "q." + ColumnName(id));
  }
  // The checks of parameters run once, before any row, as BigQuery's do when the search reads
  // none: the LEFT JOIN keeps their row.
  output.sql = "SELECT " + (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) +
               " FROM (SELECT " + ok + " AS ok) AS chk LEFT JOIN (SELECT * FROM (" + ranked +
               ") WHERE " + top_k + " < 0 OR rank <= " + top_k +
               ") AS vs ON chk.ok WHERE vs.rank IS NOT NULL";
  return output;
}

std::optional<Relation> SessionizeScan(const googlesql::ResolvedTVFScan& tvf, const Scope& scope) {
  if (const auto body = analyzer_functions::GetGraphExpandBody(*tvf.signature())) {
    return GraphExpandScan(tvf, *body, scope);
  }
  if (tvf.tvf()->Name() == "external_query") {
    return ExternalQueryScan(tvf, scope);
  }
  if (tvf.tvf()->Name() == "gap_fill") {
    return GapFillScan(tvf, scope);
  }
  if (tvf.tvf()->Name() == "vector_search") {
    return VectorSearchScan(tvf, scope);
  }
  if (tvf.tvf()->Name() != "range_sessionize") {
    return Unsupported(scope, "table function " + tvf.tvf()->Name());
  }
  const auto& argument = *tvf.argument_list(0);
  const auto input = Scan(*argument.scan(), scope);
  if (!input) {
    return std::nullopt;
  }
  const auto& schema = tvf.signature()->input_arguments().at(0).relation();
  const auto range_name = tvf.signature()->input_arguments().at(1).GetScalarArgType();
  const auto partitions = tvf.signature()->input_arguments().at(2).GetScalarArgType();
  const auto partition_names = partitions->literal_value()->elements_view();
  std::vector<std::string> keys;
  std::vector<std::string> key_projections;
  std::string range;
  for (int i = 0; i < schema.num_columns(); ++i) {
    const auto& column = argument.argument_column_list(i);
    if (!DuckDbType(column.type(), scope.context.timestamp_precision) ||
        column.type_annotation_map() != nullptr) {
      return Unsupported(scope, "RANGE_SESSIONIZE column type");
    }
    const std::string name = ColumnName(column.column_id());
    // Names were validated by the catalog, including ambiguous input names.
    if (absl::EqualsIgnoreCase(schema.column(i).name,
                               range_name->literal_value()->string_value())) {
      range = "q." + name;
    }
    for (const auto& key : *partition_names) {
      if (absl::EqualsIgnoreCase(schema.column(i).name, key.string_value())) {
        // Keep the input value for the output, and partition every window by the
        // same comparison key rather than INTERVAL's stored representation.
        const std::string key_name = "_rs_key" + std::to_string(keys.size());
        key_projections.push_back(
            std::format("{} AS {}", IntervalKey(column.type(), "q." + name), key_name));
        keys.push_back(key_name);
      }
    }
  }
  const auto& arguments = tvf.signature()->input_arguments();
  const bool overlaps =
      arguments.size() == 4 &&
      arguments.at(3).GetScalarArgType()->literal_value()->EnumDisplayName() == "OVERLAPS";
  const std::string partition = keys.empty() ? "" : "PARTITION BY " + Join(keys, ", ") + " ";
  const std::string order =
      "ORDER BY _rs_start NULLS LAST, _rs_end NULLS LAST, _rs_id "
      "ROWS BETWEEN UNBOUNDED PRECEDING AND ";
  // RANGE's unbounded ends are stored as infinities. A running maximum (rather
  // than LAG) keeps nested ranges and chains in the same connected component.
  const std::string bounds = std::format(
      "SELECT q.*, row_number() OVER () AS _rs_id, {0}.{1} AS _rs_start, "
      "{0}.{2} AS _rs_end{3}{4}",
      range, QuoteIdentifier(kRangeStart), QuoteIdentifier(kRangeEnd),
      key_projections.empty() ? "" : ", " + Join(key_projections, ", "), input->From());
  const std::string previous =
      std::format("SELECT *, max(_rs_end) OVER ({}{}1 PRECEDING) AS _rs_previous FROM ({})",
                  partition, order, bounds);
  const std::string groups = std::format(
      "SELECT *, sum(CASE WHEN _rs_previous IS NULL OR _rs_start {} _rs_previous "
      "THEN 1 ELSE 0 END) OVER ({}{}CURRENT ROW) AS _rs_group FROM ({})",
      overlaps ? ">=" : ">", partition, order, previous);
  keys.emplace_back("_rs_group");
  const std::string window = " OVER (PARTITION BY " + Join(keys, ", ") + ")";
  const std::string session = std::format(
      "CASE WHEN {} IS NULL THEN NULL ELSE "
      "struct_pack({} := min(_rs_start){}, {} := max(_rs_end){}) END",
      range, QuoteIdentifier(kRangeStart), window, QuoteIdentifier(kRangeEnd), window);
  Relation result;
  std::vector<std::string> projections;
  for (int i = 0; i < tvf.column_list_size(); ++i) {
    const int index = tvf.column_index_list(i);
    const int id = tvf.column_list(i).column_id();
    const std::string value = absl::EqualsIgnoreCase(tvf.column_list(i).name(), "session_range")
                                  ? session
                                  : ColumnName(argument.argument_column_list(index).column_id());
    projections.push_back(value + " AS " + ColumnName(id));
    result.columns.emplace(id, "q." + ColumnName(id));
  }
  result.sql = "SELECT " + (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) +
               " FROM (" + groups + ") AS q";
  return result;
}

// Bind scalar arguments in the caller's projection, once per working row. Translate the
// stored body in its own scope: its column IDs belong to a separate analyzer invocation.
std::optional<Relation> GraphCallTvf(const googlesql::ResolvedTVFScan& tvf, const Scope& scope,
                                     Relation& input, const std::string& input_alias) {
  const auto* function = dynamic_cast<const googlesql::SQLTableValuedFunction*>(tvf.tvf());
  if (function == nullptr || function->query() == nullptr || !tvf.hint_list().empty()) {
    return Unsupported(scope, "GQL CALL of a non-SQL or hinted table function");
  }
  const auto& body = *function->query();
  std::vector<const googlesql::ResolvedNode*> calls;
  body.GetDescendantsSatisfying(&googlesql::ResolvedNode::Is<googlesql::ResolvedFunctionCallBase>,
                                &calls);
  for (const auto* node : calls) {
    const auto* call = node->GetAs<googlesql::ResolvedFunctionCallBase>();
    if (call->function()->function_options().volatility == googlesql::FunctionEnums::VOLATILE) {
      // DuckDB can evaluate a lateral right side just once for equal arguments.
      return Unsupported(scope, "volatile SQL table function body in GQL CALL");
    }
  }
  // The body is not part of the caller's partition-filter or field-use analysis.
  std::vector<const googlesql::ResolvedNode*> tables;
  body.GetDescendantsWithKinds({googlesql::RESOLVED_TABLE_SCAN}, &tables);
  for (const auto* table : tables) {
    const auto* stored =
        dynamic_cast<const BigQueryTable*>(table->GetAs<googlesql::ResolvedTableScan>()->table());
    const auto description = stored == nullptr ? std::nullopt : stored->Describe();
    if (stored != nullptr &&
        (stored->ReadQuery() || (description && description->metadata.require_partition_filter))) {
      return Unsupported(scope, "GQL CALL reading external storage or required partition filters");
    }
  }
  Scope nested{.context = scope.context};
  const auto names = function->GetArgumentNames();
  std::vector<std::string> arguments;
  for (int i = 0; i < tvf.argument_list_size(); ++i) {
    const auto* expr = tvf.argument_list(i)->expr();
    if (expr == nullptr) {
      return Unsupported(scope, "non-scalar table function argument in GQL CALL");
    }
    const auto sql = Expression(*expr, scope, {});
    if (!sql) {
      return std::nullopt;
    }
    const auto name = QuoteIdentifier(scope.context.FreshName("call_arg"));
    arguments.push_back(*sql + " AS " + name);
    nested.table_function_arguments.emplace(absl::AsciiStrToLower(names.at(i)),
                                            std::format("{}.{}", input_alias, name));
  }
  if (!arguments.empty()) {
    input.sql = std::format("SELECT {}.*, {} FROM ({}) AS {}", input_alias, Join(arguments, ", "),
                            input.sql, input_alias);
  }
  const auto relation = Scan(body, nested);
  if (!relation) {
    return std::nullopt;
  }
  Relation result;
  std::vector<std::string> projections;
  for (int i = 0; i < tvf.column_list_size(); ++i) {
    const auto name = ColumnName(tvf.column_list(i).column_id());
    const auto source = body.column_list(tvf.column_index_list(i)).column_id();
    projections.push_back(relation->columns.at(source) + " AS " + name);
    result.columns.emplace(tvf.column_list(i).column_id(), "q." + name);
  }
  result.sql =
      "SELECT " + (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) + relation->From();
  return result;
}

// Projects `columns` of a relation, positionally renamed to `names`.
std::optional<std::vector<std::string>> Renamed(
    const Relation& relation, const std::vector<googlesql::ResolvedColumn>& columns,
    const std::vector<std::string>& names) {
  std::vector<std::string> projections;
  for (size_t i = 0; i < columns.size(); ++i) {
    const auto column = relation.columns.find(columns.at(i).column_id());
    if (column == relation.columns.end()) {
      return std::nullopt;
    }
    projections.push_back(column->second + " AS " + names.at(i));
  }
  return projections;
}

std::optional<Relation> JoinScan(const googlesql::ResolvedJoinScan& join, const Scope& scope) {
  const auto left = Scan(*join.left_scan(), scope);
  if (!left) {
    return std::nullopt;
  }
  // A lateral right side sees the left's columns the way a correlated subquery sees its
  // enclosing query's.
  const auto right =
      Scan(*join.right_scan(), join.is_lateral() ? Nested(scope, left->columns) : scope);
  if (!right) {
    return std::nullopt;
  }
  Relation result;
  Columns visible;
  std::vector<std::string> projections;
  for (const auto& [side, relation] : {std::pair{"l.", &*left}, std::pair{"r.", &*right}}) {
    for (const auto& [id, sql] : relation->columns) {
      visible.emplace(id, side + ColumnName(id));
      projections.push_back(side + ColumnName(id) + " AS " + ColumnName(id));
      result.columns.emplace(id, "q." + ColumnName(id));
    }
  }
  std::string kind;
  switch (join.join_type()) {
    case googlesql::ResolvedJoinScan::INNER:
      kind = "INNER";
      break;
    case googlesql::ResolvedJoinScan::LEFT:
      kind = "LEFT";
      break;
    case googlesql::ResolvedJoinScan::RIGHT:
      kind = "RIGHT";
      break;
    case googlesql::ResolvedJoinScan::FULL:
      kind = "FULL";
      break;
    default:
      return Unsupported(scope, "join type");
  }
  std::string condition = " ON TRUE";
  if (join.join_expr() != nullptr) {
    const auto on = Expression(*join.join_expr(), scope, visible);
    if (!on) {
      return std::nullopt;
    }
    condition = " ON " + *on;
  } else if (kind == "INNER") {
    kind = "CROSS";
    condition.clear();
  }
  result.sql = "SELECT " + (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) +
               " FROM (" + left->sql + ") AS l " + kind + " JOIN " +
               (join.is_lateral() ? "LATERAL " : "") + "(" + right->sql + ") AS r" + condition;
  return result;
}

// The body of a recursive WITH entry: DuckDB evaluates UNION [ALL] in WITH RECURSIVE by the
// same iteration, deduplicating against every earlier row for UNION.
std::optional<std::string> RecursiveQuery(const googlesql::ResolvedRecursiveScan& recursive,
                                          const std::vector<std::string>& names,
                                          const WithQuery& self, const Scope& scope) {
  if (recursive.recursion_depth_modifier() != nullptr) {
    return Unsupported(scope, "WITH RECURSIVE depth modifier");
  }
  // UNION compares rows by their columns' collations, which the resolved AST does not record.
  if (recursive.op_type() != googlesql::ResolvedRecursiveScan::UNION_ALL &&
      std::ranges::any_of(recursive.column_list(), [](const googlesql::ResolvedColumn& column) {
        return HasCollation(column.type_annotation_map());
      })) {
    return Unsupported(scope, "WITH RECURSIVE UNION DISTINCT with collation");
  }
  std::vector<std::string> terms;
  for (const auto* item : {recursive.non_recursive_term(), recursive.recursive_term()}) {
    Scope inner = scope;
    if (item == recursive.recursive_term()) {
      inner.recursive = self;
    }
    const auto relation = Scan(*item->scan(), inner);
    if (!relation) {
      return std::nullopt;
    }
    const auto projections = Renamed(*relation, item->output_column_list(), names);
    if (!projections || projections->empty()) {
      return std::nullopt;
    }
    terms.push_back("SELECT " + Join(*projections, ", ") + relation->From());
  }
  return Join(terms, recursive.op_type() == googlesql::ResolvedRecursiveScan::UNION_ALL
                         ? " UNION ALL "
                         : " UNION ");
}

std::optional<Relation> WithScan(const googlesql::ResolvedWithScan& with, const Scope& scope) {
  Scope inner = scope;
  std::vector<std::string> definitions;
  for (const auto& entry : with.with_entry_list()) {
    const auto* subquery = entry->with_subquery();
    std::vector<std::string> names;
    names.reserve(subquery->column_list_size());
    for (int i = 0; i < subquery->column_list_size(); ++i) {
      names.push_back(QuoteIdentifier("_p" + std::to_string(i)));
    }
    const WithQuery query{.name = scope.context.FreshName("_w"), .width = names.size()};
    std::optional<std::string> body;
    if (subquery->Is<googlesql::ResolvedRecursiveScan>()) {
      body =
          RecursiveQuery(*subquery->GetAs<googlesql::ResolvedRecursiveScan>(), names, query, inner);
    } else {
      const auto relation = Scan(*subquery, inner);
      if (!relation) {
        return std::nullopt;
      }
      const auto projections = Renamed(*relation, subquery->column_list(), names);
      if (!projections || projections->empty()) {
        return std::nullopt;
      }
      body = "SELECT " + Join(*projections, ", ") + relation->From();
    }
    if (!body) {
      return std::nullopt;
    }
    definitions.push_back(
        std::format("{}({}) AS ({})", QuoteIdentifier(query.name), Join(names, ", "), *body));
    inner.with[entry->with_query_name()] = query;
  }
  auto result = Scan(*with.query(), inner);
  if (!result) {
    return std::nullopt;
  }
  result->sql = std::string(with.recursive() ? "WITH RECURSIVE " : "WITH ") +
                Join(definitions, ", ") + " SELECT q.*" + result->From();
  return result;
}

// Reads `query` into fresh columns.
Relation WithRead(const WithQuery& query, const std::vector<googlesql::ResolvedColumn>& columns) {
  Relation result;
  std::vector<std::string> projections;
  for (size_t i = 0; i < columns.size(); ++i) {
    const int id = columns.at(i).column_id();
    projections.push_back("q." + QuoteIdentifier("_p" + std::to_string(i)) + " AS " +
                          ColumnName(id));
    result.columns.emplace(id, "q." + ColumnName(id));
  }
  result.sql =
      std::format("SELECT {} FROM {} AS q", Join(projections, ", "), QuoteIdentifier(query.name));
  return result;
}

std::optional<Relation> WithRefScan(const googlesql::ResolvedWithRefScan& ref, const Scope& scope) {
  const auto with = scope.with.find(ref.with_query_name());
  if (with == scope.with.end() || std::cmp_not_equal(with->second.width, ref.column_list_size())) {
    return std::nullopt;
  }
  return WithRead(with->second, ref.column_list());
}

std::optional<Relation> RecursiveRefScan(const googlesql::ResolvedRecursiveRefScan& ref,
                                         const Scope& scope) {
  if (!scope.recursive || std::cmp_not_equal(scope.recursive->width, ref.column_list_size())) {
    return std::nullopt;
  }
  return WithRead(*scope.recursive, ref.column_list());
}

std::optional<Relation> SetOperationScan(const googlesql::ResolvedSetOperationScan& set,
                                         const Scope& scope) {
  // CORRESPONDING is resolved into each input item's output_column_list, which is positional
  // and pads missing columns with NULLs, so the match and propagation modes need no handling.
  using Operator =
      std::pair<googlesql::ResolvedSetOperationScan::SetOperationType, std::string_view>;
  static constexpr auto operators = std::to_array<Operator>({
      {googlesql::ResolvedSetOperationScan::UNION_ALL, " UNION ALL "},
      {googlesql::ResolvedSetOperationScan::UNION_DISTINCT, " UNION "},
      {googlesql::ResolvedSetOperationScan::INTERSECT_DISTINCT, " INTERSECT "},
      {googlesql::ResolvedSetOperationScan::EXCEPT_DISTINCT, " EXCEPT "},
      {googlesql::ResolvedSetOperationScan::INTERSECT_ALL, " INTERSECT ALL "},
      {googlesql::ResolvedSetOperationScan::EXCEPT_ALL, " EXCEPT ALL "},
  });
  const bool all = set.op_type() == googlesql::ResolvedSetOperationScan::INTERSECT_ALL ||
                   set.op_type() == googlesql::ResolvedSetOperationScan::EXCEPT_ALL;
  // GoogleSQL accepts these forms, but BigQuery only supports DISTINCT for them.
  if (all && scope.context.functions.restrictions().intersect_except_distinct_only) {
    const std::string name = set.op_type() == googlesql::ResolvedSetOperationScan::INTERSECT_ALL
                                 ? "INTERSECT"
                                 : "EXCEPT";
    throw std::invalid_argument(name + " ALL is not supported. Only " + name +
                                " DISTINCT is supported.");
  }
  const auto* op = std::ranges::find(operators, set.op_type(), &Operator::first);
  if (op == operators.end() || set.column_list().empty()) {
    return std::nullopt;
  }
  const bool distinct = set.op_type() == googlesql::ResolvedSetOperationScan::UNION_DISTINCT ||
                        set.op_type() == googlesql::ResolvedSetOperationScan::INTERSECT_DISTINCT ||
                        set.op_type() == googlesql::ResolvedSetOperationScan::EXCEPT_DISTINCT;
  Relation result;
  std::vector<std::string> names;
  // Whether DISTINCT compares a column by keys: INTERVAL values by their lengths, and strings by
  // their collation keys under the column's collation, which the resolved AST does not record.
  bool keys = false;
  std::vector<Collation> collations;
  for (const auto& column : set.column_list()) {
    if (distinct &&
        ((column.type()->IsArray() &&
          scope.context.functions.restrictions().distinct_set_operations_without_arrays) ||
         (column.type()->IsStruct() &&
          scope.context.functions.restrictions().distinct_set_operations_without_structs))) {
      const std::string name = set.op_type() == googlesql::ResolvedSetOperationScan::UNION_DISTINCT
                                   ? "UNION DISTINCT"
                                   : "INTERSECT DISTINCT or EXCEPT DISTINCT";
      throw std::invalid_argument(std::string("Type ") +
                                  (column.type()->IsStruct() ? "STRUCT" : "ARRAY") +
                                  " is not supported in " + name + " operation");
    }
    collations.push_back(ColumnCollation(column.type_annotation_map()));
    keys = keys || HasInterval(column.type()) || !collations.back().empty();
    names.push_back(ColumnName(column.column_id()));
    result.columns.emplace(column.column_id(), "q." + names.back());
  }
  std::vector<std::string> parts;
  for (const auto& item : set.input_item_list()) {
    const auto relation = Scan(*item->scan(), scope);
    if (!relation || item->output_column_list().size() != names.size()) {
      return std::nullopt;
    }
    const auto projections = Renamed(*relation, item->output_column_list(), names);
    if (!projections) {
      return std::nullopt;
    }
    parts.push_back("SELECT " + Join(*projections, ", ") + relation->From());
  }
  if (set.op_type() == googlesql::ResolvedSetOperationScan::UNION_ALL || !keys) {
    result.sql = Join(parts, op->second);
    return result;
  }
  if (all) {
    return Unsupported(scope, "INTERSECT ALL and EXCEPT ALL of INTERVAL or collated columns");
  }
  // Each distinct row is the first input row of its keys, as BigQuery returns it.
  const auto key = [&](int i, const std::string& sql) {
    const auto* type = set.column_list(i).type();
    return CollationKey(IntervalKey(type, sql), collations.at(i), type);
  };
  std::vector<std::string> compared;
  std::vector<std::string> keyed;
  std::vector<std::string> matches;
  for (int i = 0; i < set.column_list_size(); ++i) {
    const std::string& name = names.at(i);
    compared.push_back(key(i, "r." + name));
    keyed.push_back(std::format("{} AS {}", key(i, "q." + name), name));
    matches.push_back(std::format("k.{} IS NOT DISTINCT FROM {}", name, compared.back()));
  }
  std::vector<std::string> keyed_parts;
  std::vector<std::string> rows;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    keyed_parts.push_back(std::format("SELECT {} FROM ({}) AS q", Join(keyed, ", "), parts.at(i)));
    rows.push_back(std::format("SELECT q.*, {} AS _sb, row_number() OVER () AS _sr FROM ({}) AS q",
                               i, parts.at(i)));
  }
  std::string where = "r._sn = 1";
  if (set.op_type() != googlesql::ResolvedSetOperationScan::UNION_DISTINCT) {
    where += std::format(" AND EXISTS (SELECT 1 FROM ({}) AS k WHERE {})",
                         Join(keyed_parts, op->second), Join(matches, " AND "));
  }
  std::vector<std::string> outputs;
  std::ranges::transform(names, std::back_inserter(outputs),
                         [](const std::string& name) { return "r." + name; });
  result.sql = std::format(
      "SELECT {} FROM (SELECT r.*, row_number() OVER (PARTITION BY {} ORDER BY r._sb, r._sr) AS "
      "_sn FROM ({}) AS r) AS r WHERE {}",
      Join(outputs, ", "), Join(compared, ", "), Join(rows, " UNION ALL "), where);
  return result;
}

// The joined tail of an array scan: the lateral UNNEST `unnest` with its element and offset
// columns `visible` to the join condition.
std::optional<Relation> JoinArrays(const googlesql::ResolvedArrayScan& array, Relation input,
                                   const std::string& unnest, Columns visible,
                                   const std::vector<std::string>& elements, const Scope& scope) {
  std::vector<std::string> projections = {"q.*"};
  for (int i = 0; i < array.element_column_list_size(); ++i) {
    const int element = array.element_column_list(i).column_id();
    projections.push_back(elements.at(i) + " AS " + ColumnName(element));
    visible.emplace(element, elements.at(i));
    input.columns.emplace(element, "q." + ColumnName(element));
  }
  if (array.array_offset_column() != nullptr) {
    const int offset = array.array_offset_column()->column().column_id();
    projections.push_back("CAST(u.o - 1 AS BIGINT) AS " + ColumnName(offset));
    visible.emplace(offset, "(u.o - 1)");
    input.columns.emplace(offset, "q." + ColumnName(offset));
  }
  std::string condition = "TRUE";
  if (array.join_expr() != nullptr) {
    const auto on = Expression(*array.join_expr(), scope, visible);
    if (!on) {
      return std::nullopt;
    }
    condition = *on;
  }
  input.sql = "SELECT " + Join(projections, ", ") + input.From() +
              (array.is_outer() ? " LEFT" : " INNER") + " JOIN LATERAL " + unnest + " ON " +
              condition;
  return input;
}

// UNNEST(a, b, mode => ...) walks the arrays in step by position. The row count is the longest
// array's for PAD, which pads the others with NULL, and the shortest's for TRUNCATE; STRICT
// fails when the lengths differ. A NULL array counts as empty.
std::optional<Relation> ZippedArrayScan(const googlesql::ResolvedArrayScan& array,
                                        const Relation& input, const Scope& scope) {
  std::string mode = "PAD";
  if (const auto* zip = array.array_zip_mode(); zip != nullptr) {
    if (!zip->Is<googlesql::ResolvedLiteral>() ||
        zip->GetAs<googlesql::ResolvedLiteral>()->value().is_null() || !zip->type()->IsEnum()) {
      return Unsupported(scope, "UNNEST mode that is not a literal");
    }
    mode = zip->GetAs<googlesql::ResolvedLiteral>()->value().EnumDisplayName();
  }
  std::vector<std::string> arrays;
  std::vector<std::string> lengths;
  std::vector<std::string> elements;
  std::vector<std::string> picks;
  for (int i = 0; i < array.array_expr_list_size(); ++i) {
    const auto sql = Expression(*array.array_expr_list(i), scope, input.columns);
    if (!sql) {
      return std::nullopt;
    }
    const std::string name = "a" + std::to_string(i);
    arrays.push_back(*sql + " AS " + name);
    lengths.push_back("coalesce(len(z." + name + "), 0)");
    elements.push_back("u.e" + std::to_string(i));
    picks.push_back("z." + name + "[u.o] AS e" + std::to_string(i));
  }
  std::string count;
  if (mode == "PAD") {
    count = "greatest(" + Join(lengths, ", ") + ")";
  } else if (mode == "TRUNCATE") {
    count = "least(" + Join(lengths, ", ") + ")";
  } else if (mode == "STRICT") {
    count = std::format(
        "CASE WHEN least({0}) = greatest({0}) THEN greatest({0})"
        " ELSE error('Unnested arrays under STRICT mode must have equal lengths') END",
        Join(lengths, ", "));
  } else {
    return Unsupported(scope, "UNNEST mode " + mode);
  }
  picks.emplace_back("u.o");
  const std::string unnest =
      std::format("(SELECT {} FROM (SELECT {}) AS z, range(1, {} + 1) AS u(o)) AS u",
                  Join(picks, ", "), Join(arrays, ", "), count);
  return JoinArrays(array, input, unnest, input.columns, elements, scope);
}

std::optional<Relation> ArrayScan(const googlesql::ResolvedArrayScan& array, const Scope& scope) {
  auto result = array.input_scan() == nullptr
                    ? std::optional<Relation>(
                          Relation{.sql = "SELECT 1 AS _unit", .columns = {}, .ordering = {}})
                    : Scan(*array.input_scan(), scope);
  if (!result) {
    return std::nullopt;
  }
  if (array.array_expr_list_size() != 1) {
    return ZippedArrayScan(array, *result, scope);
  }
  const auto elements = Expression(*array.array_expr_list(0), scope, result->columns);
  if (!elements) {
    return std::nullopt;
  }
  // UNNEST is joined laterally so the array can refer to the input row. Ordinals are 1-based.
  return JoinArrays(array, *result, "unnest(" + *elements + ") WITH ORDINALITY AS u(e, o)",
                    result->columns, {"u.e"}, scope);
}

std::optional<Relation> SampleScan(const googlesql::ResolvedSampleScan& sample,
                                   const Scope& scope) {
  if (sample.method() != "system" || sample.unit() != googlesql::ResolvedSampleScan::PERCENT ||
      sample.repeatable_argument() != nullptr || sample.weight_column() != nullptr ||
      !sample.partition_by_list().empty()) {
    return Unsupported(scope, "TABLESAMPLE other than SYSTEM (PERCENT) without a suffix");
  }
  if (!sample.input_scan()->Is<googlesql::ResolvedTableScan>()) {
    return Unsupported(scope, "TABLESAMPLE on a subquery or array");
  }
  const auto& size = *sample.size();
  std::optional<googlesql::Value> value;
  if (size.Is<googlesql::ResolvedLiteral>()) {
    value = size.GetAs<googlesql::ResolvedLiteral>()->value();
  } else if (size.Is<googlesql::ResolvedParameter>() && scope.context.defaults.evaluate) {
    // DuckDB requires a numeric literal here, so evaluate the typed parameter first, as DDL
    // option values do, rather than extracting a number from its generated SQL.
    const auto sql = Expression(size, scope, {});
    if (!sql) {
      return std::nullopt;
    }
    value = scope.context.defaults.evaluate("SELECT " + *sql, size.type());
  } else {
    return Unsupported(scope, "TABLESAMPLE percentage");
  }
  if (value->is_null()) {
    throw std::invalid_argument("Const must be a DOUBLE-compatible constant.");
  }
  double percent =
      value->type()->IsInt64() ? static_cast<double>(value->int64_value()) : value->double_value();
  // BigQuery returned every row for a NaN parameter, including across multiple partitions.
  if (std::isnan(percent)) {
    percent = 100;
  }
  // The analyzer validates literals, but parameter values still need checking.
  if (percent < 0 || percent > 100) {
    throw std::invalid_argument("Sampling rate must be between 0 and 100 percent.");
  }
  auto result = Scan(*sample.input_scan(), scope);
  if (!result) {
    return std::nullopt;
  }
  // SYSTEM samples DuckDB vectors instead of BigQuery storage blocks; see README's exceptions.
  result->sql =
      "SELECT q.*" + result->From() + std::format(" TABLESAMPLE SYSTEM ({} PERCENT)", percent);
  return result;
}

// Whether `expression` has the same value in every time zone. BigQuery evaluates the time of FOR
// SYSTEM_TIME AS OF in America/Los_Angeles rather than in UTC, which the analyzer has already
// given a literal's time, so the emulator reads only times that do not depend on the zone.
bool ZoneIndependent(const googlesql::ResolvedExpr& expression) {
  if (expression.Is<googlesql::ResolvedParameter>() ||
      expression.Is<googlesql::ResolvedConstant>()) {
    return true;
  }
  if (expression.Is<googlesql::ResolvedLiteral>()) {
    return !expression.type()->IsTimestamp() ||
           expression.GetAs<googlesql::ResolvedLiteral>()->value().is_null();
  }
  if (!expression.Is<googlesql::ResolvedFunctionCall>()) {
    return false;
  }
  const auto& call = *expression.GetAs<googlesql::ResolvedFunctionCall>();
  const std::string name = call.function()->Name();
  return std::ranges::find(kZoneIndependentFunctions, name) != kZoneIndependentFunctions.end() &&
         std::ranges::all_of(call.argument_list(),
                             [](const auto& argument) { return ZoneIndependent(*argument); });
}

}  // namespace

std::optional<std::string> TimeTravelRows(const googlesql::ResolvedTableScan& scan,
                                          const Scope& scope, int64_t& time) {
  const auto* stored = dynamic_cast<const BigQueryTable*>(scan.table());
  TableSource* source = scope.context.defaults.source;
  const auto& expression = *scan.for_system_time_expr();
  if (stored == nullptr || source == nullptr || !scope.context.defaults.evaluate ||
      !expression.type()->IsTimestamp()) {
    return Unsupported(scope, "FOR SYSTEM_TIME AS OF this table");
  }
  if (stored->WriteRestriction() == TableWriteRestriction::kExternalNumericStorage) {
    return Unsupported(scope, "FOR SYSTEM_TIME AS OF external numeric storage");
  }
  if (!ZoneIndependent(expression)) {
    return Unsupported(scope,
                       "FOR SYSTEM_TIME AS OF a time other than a parameter, a variable or "
                       "CURRENT_TIMESTAMP with TIMESTAMP_ADD or TIMESTAMP_SUB");
  }
  // A query of the view reads the table at the time it evaluates, not at the view's creation.
  if (scope.context.defines_view) {
    return "SELECT * FROM " + QuoteIdentifierPath(stored->FullName()) +
           " WHERE error('The emulator reads FOR SYSTEM_TIME AS OF in a view only through its "
           "GoogleSQL query')";
  }
  // The time is a constant, which the emulator evaluates to find the rows the table had then.
  const auto sql = Expression(expression, scope, {});
  if (!sql) {
    return std::nullopt;
  }
  const googlesql::Value value =
      scope.context.defaults.evaluate("SELECT " + *sql, expression.type());
  if (value.is_null()) {
    throw std::invalid_argument("Query error: 'FOR SYSTEM_TIME AS OF' expression for table '" +
                                stored->reference().project_id + "." +
                                stored->reference().dataset_id + "." +
                                stored->reference().table_id + "' is NULL.");
  }
  time = absl::ToUnixMicros(value.ToUnixPicos().ToAbslTime());
  absl::StatusOr<std::string> rows = source->TimeTravelQuery(stored->reference(), time);
  if (absl::IsUnimplemented(rows.status())) {
    return Unsupported(scope, rows.status().message());
  }
  if (!rows.ok()) {
    throw std::invalid_argument(std::string(rows.status().message()));
  }
  return *std::move(rows);
}

namespace {

// A single unfiltered MATCH followed by an ungrouped COUNT(*) observes one row per
// endpoint pair, not which path ANY CHEAPEST selects. Do not pass this through other
// GQL steps: they can inspect the chosen path even when the final result is a count.
bool CountsGraphRows(const googlesql::ResolvedScan& scan) {
  const auto* input = &scan;
  while (const auto* project = dynamic_cast<const googlesql::ResolvedProjectScan*>(input)) {
    input = project->input_scan();
  }
  const auto* aggregate = dynamic_cast<const googlesql::ResolvedAggregateScan*>(input);
  if (aggregate == nullptr || !aggregate->input_scan()->Is<googlesql::ResolvedGraphRefScan>() ||
      !aggregate->group_by_list().empty() || !aggregate->grouping_set_list().empty() ||
      aggregate->aggregate_list_size() != 1) {
    return false;
  }
  const auto* count = dynamic_cast<const googlesql::ResolvedAggregateFunctionCall*>(
      aggregate->aggregate_list(0)->expr());
  return count != nullptr && count->function()->Name() == "$count_star";
}

std::optional<Relation> ScanBody(const googlesql::ResolvedScan& scan, const Scope& scope) {
  if (scan.Is<googlesql::ResolvedGraphTableScan>()) {
    return GraphTableScan(*scan.GetAs<googlesql::ResolvedGraphTableScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedGraphScan>()) {
    return GraphScan(*scan.GetAs<googlesql::ResolvedGraphScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedGraphRefScan>()) {
    if (!scope.graph_input) {
      return Unsupported(scope, "graph input reference");
    }
    return *scope.graph_input;
  }
  if (scan.Is<googlesql::ResolvedGraphLinearScan>()) {
    Scope linear = scope;
    const auto& steps = scan.GetAs<googlesql::ResolvedGraphLinearScan>()->scan_list();
    const auto* match = steps.size() == 2 && CountsGraphRows(*steps.back())
                            ? dynamic_cast<const googlesql::ResolvedGraphScan*>(steps.front().get())
                            : nullptr;
    for (const auto& step : steps) {
      auto result = step.get() == match && step->hint_list().empty()
                        ? GraphScan(*match, linear, /*count_only=*/true)
                        : Scan(*step, linear);
      if (!result) {
        return std::nullopt;
      }
      linear.graph_input = std::make_shared<Relation>(*std::move(result));
    }
    return linear.graph_input ? std::optional<Relation>(*linear.graph_input) : std::nullopt;
  }
  if (scan.Is<googlesql::ResolvedGraphCallScan>()) {
    const auto& call = *scan.GetAs<googlesql::ResolvedGraphCallScan>();
    const auto* tvf = dynamic_cast<const googlesql::ResolvedTVFScan*>(call.subquery());
    if (tvf == nullptr && !call.subquery()->Is<googlesql::ResolvedGraphTableScan>()) {
      return Unsupported(scope, "GQL CALL of a rewritten table function");
    }
    auto input = Scan(*call.input_scan(), scope);
    if (!input) {
      return std::nullopt;
    }
    const auto left = QuoteIdentifier(scope.context.FreshName("call_input"));
    const auto right = QuoteIdentifier(scope.context.FreshName("call_result"));
    Scope nested = scope;
    // The inline query has its own working table; only declared correlated columns
    // can see the caller. Fresh aliases also keep nested CALLs from shadowing it.
    // A parameter the input does not produce, such as a column of an enclosing CALL
    // or query, keeps the reference the enclosing scope gives it.
    nested.graph_input.reset();
    for (const auto& parameter : call.parameter_list()) {
      const int id = parameter->column().column_id();
      if (input->columns.contains(id)) {
        nested.outer[id] = left + "." + ColumnName(id);
      }
    }
    const auto subquery =
        tvf == nullptr ? Scan(*call.subquery(), nested) : GraphCallTvf(*tvf, nested, *input, left);
    if (!subquery) {
      return std::nullopt;
    }
    Relation result;
    std::vector<std::string> projections;
    for (const auto& column : call.column_list()) {
      const int id = column.column_id();
      const auto& alias = input->columns.contains(id) ? left : right;
      projections.push_back(alias + "." + ColumnName(id) + " AS " + ColumnName(id));
      result.columns.emplace(id, "q." + ColumnName(id));
    }
    result.sql = "SELECT " + (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) +
                 " FROM (" + input->sql + ") AS " + left +
                 (call.optional() ? " LEFT JOIN LATERAL (" : " INNER JOIN LATERAL (") +
                 subquery->sql + ") AS " + right + " ON TRUE";
    return result;
  }
  if (scan.Is<googlesql::ResolvedSingleRowScan>()) {
    return Relation{.sql = "SELECT 1 AS _unit", .columns = {}, .ordering = {}};
  }
  if (scan.Is<googlesql::ResolvedTableScan>()) {
    const auto* table = scan.GetAs<googlesql::ResolvedTableScan>();
    if (table->lock_mode() != nullptr || table->read_as_row_type() ||
        table->table()->IsValueTable() ||
        table->column_list_size() != table->column_index_list_size()) {
      return Unsupported(scope, "table read with a lock mode or a value table");
    }
    std::optional<std::string> history;
    if (table->for_system_time_expr() != nullptr) {
      int64_t time = 0;
      history = TimeTravelRows(*table, scope, time);
      if (!history) {
        return std::nullopt;
      }
    }
    Relation result;
    std::vector<std::string> projections;
    std::set<std::string> names;
    ReadFields fields;
    for (int i = 0; i < table->column_list_size(); ++i) {
      const auto& column = table->column_list(i);
      if (!DuckDbType(column.type(), scope.context.timestamp_precision) ||
          !OnlyCollations(column.type_annotation_map())) {
        return Unsupported(scope, "column type " + column.type()->DebugString());
      }
      const std::string alias = ColumnName(column.column_id());
      const std::string name = table->table()->GetColumn(table->column_index_list(i))->Name();
      names.insert(name);
      if (const auto found = scope.context.read_fields.find(column.column_id());
          found != scope.context.read_fields.end()) {
        fields.emplace(name, found->second);
      }
      projections.push_back(
          StoredColumn(QuoteIdentifier(name), column.type(), scope.context.timestamp_precision) +
          " AS " + alias);
      result.columns.emplace(column.column_id(), "q." + alias);
    }
    // A view such as INFORMATION_SCHEMA.TABLES is read from the query that computes it.
    const auto* sql_table = dynamic_cast<const SqlTable*>(table->table());
    const auto* stored_table = dynamic_cast<const BigQueryTable*>(table->table());
    const auto read_query = [&]() -> std::optional<std::string> {
      if (history || stored_table == nullptr) {
        return std::nullopt;
      }
      std::optional<TableRead> definition = stored_table->ReadQuery();
      // A view's DuckDB definition cannot hold a read that one statement prepares, such as of
      // an external table's files: queries of the view read the table through its GoogleSQL
      // query.
      if (scope.context.defines_view && definition &&
          !std::holds_alternative<std::string>(*definition)) {
        return "SELECT * FROM " + QuoteIdentifierPath(table->table()->FullName()) +
               " WHERE error('The emulator reads an external table in a view only through its "
               "GoogleSQL query')";
      }
      return StatementResources::ReadQuery(stored_table->reference(), definition,
                                           scope.context.resources, names, fields);
    }();
    result.sql = "SELECT " + (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) +
                 " FROM " +
                 (history                ? "(" + *history + ") AS v"
                  : sql_table != nullptr ? "(" + sql_table->sql() + ") AS v"
                  : read_query           ? "(" + *read_query + ") AS v"
                                         : QuoteIdentifierPath(table->table()->FullName()));
    return result;
  }
  if (scan.Is<googlesql::ResolvedTVFScan>()) {
    return SessionizeScan(*scan.GetAs<googlesql::ResolvedTVFScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedJoinScan>()) {
    return JoinScan(*scan.GetAs<googlesql::ResolvedJoinScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedPivotScan>()) {
    return PivotScan(*scan.GetAs<googlesql::ResolvedPivotScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedAggregateScan>()) {
    return AggregateScan(*scan.GetAs<googlesql::ResolvedAggregateScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedDifferentialPrivacyAggregateScan>()) {
    return DifferentialPrivacyAggregateScan(
        *scan.GetAs<googlesql::ResolvedDifferentialPrivacyAggregateScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedAnalyticScan>()) {
    return AnalyticScan(*scan.GetAs<googlesql::ResolvedAnalyticScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedWithScan>()) {
    return WithScan(*scan.GetAs<googlesql::ResolvedWithScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedRecursiveRefScan>()) {
    return RecursiveRefScan(*scan.GetAs<googlesql::ResolvedRecursiveRefScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedWithRefScan>()) {
    return WithRefScan(*scan.GetAs<googlesql::ResolvedWithRefScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedSetOperationScan>()) {
    return SetOperationScan(*scan.GetAs<googlesql::ResolvedSetOperationScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedArrayScan>()) {
    return ArrayScan(*scan.GetAs<googlesql::ResolvedArrayScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedSampleScan>()) {
    return SampleScan(*scan.GetAs<googlesql::ResolvedSampleScan>(), scope);
  }
  const googlesql::ResolvedScan* input = nullptr;
  if (scan.Is<googlesql::ResolvedProjectScan>()) {
    input = scan.GetAs<googlesql::ResolvedProjectScan>()->input_scan();
  }
  if (scan.Is<googlesql::ResolvedFilterScan>()) {
    input = scan.GetAs<googlesql::ResolvedFilterScan>()->input_scan();
  }
  if (scan.Is<googlesql::ResolvedOrderByScan>()) {
    input = scan.GetAs<googlesql::ResolvedOrderByScan>()->input_scan();
  }
  if (scan.Is<googlesql::ResolvedLimitOffsetScan>()) {
    input = scan.GetAs<googlesql::ResolvedLimitOffsetScan>()->input_scan();
  }
  if (input == nullptr) {
    return Unsupported(scope, "scan " + scan.node_kind_string());
  }
  auto result = Scan(*input, scope);
  if (!result) {
    return std::nullopt;
  }
  const std::string from = result->From();
  if (scan.Is<googlesql::ResolvedProjectScan>()) {
    const auto* project = scan.GetAs<googlesql::ResolvedProjectScan>();
    const Columns input_columns = result->columns;
    std::vector<std::string> projections;
    for (const auto& [id, sql] : input_columns) {
      projections.push_back(sql + " AS " + ColumnName(id));
    }
    for (const auto& computed : project->expr_list()) {
      const auto expression = Expression(*computed->expr(), scope, input_columns);
      if (!expression) {
        return std::nullopt;
      }
      const int id = computed->column().column_id();
      projections.push_back(*expression + " AS " + ColumnName(id));
      result->columns.emplace(id, "q." + ColumnName(id));
    }
    result->sql = "SELECT " + (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) + from;
  } else if (scan.Is<googlesql::ResolvedFilterScan>()) {
    const auto filter = Expression(*scan.GetAs<googlesql::ResolvedFilterScan>()->filter_expr(),
                                   scope, result->columns);
    if (!filter) {
      return std::nullopt;
    }
    result->sql = "SELECT q.*" + from + " WHERE " + *filter;
  } else if (scan.Is<googlesql::ResolvedOrderByScan>()) {
    const auto order = OrderItems(
        scan.GetAs<googlesql::ResolvedOrderByScan>()->order_by_item_list(), scope, result->columns);
    if (!order) {
      return std::nullopt;
    }
    result->ordering = {*order};
  } else {
    const auto* limit = scan.GetAs<googlesql::ResolvedLimitOffsetScan>();
    const auto count = Expression(*limit->limit(), scope, {});
    if (!count) {
      return std::nullopt;
    }
    // DuckDB rejects a count or offset above 2^62, which no result reaches, so capping it there
    // returns what BigQuery does.
    const auto checked = [](const std::string& sql) {
      return "least(bq_checked_integer(" + sql +
             ", 0, 'Limit requires non-null count and offset', "
             "'Limit requires non-negative count and offset'), 4611686018427387904)";
    };
    result->sql = "SELECT q.*" + from + result->Order() + " LIMIT " + checked(*count);
    if (limit->offset() != nullptr) {
      const auto offset = Expression(*limit->offset(), scope, {});
      if (!offset) {
        return std::nullopt;
      }
      result->sql += " OFFSET " + checked(*offset);
    }
  }
  return result;
}

}  // namespace

std::optional<Relation> Scan(const googlesql::ResolvedScan& scan, const Scope& scope) {
  if (!scan.hint_list().empty()) {
    return Unsupported(scope, "hints");
  }
  auto result = ScanBody(scan, scope);
  // Ordering from a subquery is not a promise of ordering for its parent scan. A GQL query's
  // ORDER BY marks only its OrderByScan. The next statement's reference to it, such as LIMIT's,
  // and the linear and graph table scans that output it keep its order, as the reference
  // implementation's do.
  if (result && !scan.is_ordered() && !scan.Is<googlesql::ResolvedGraphRefScan>() &&
      !scan.Is<googlesql::ResolvedGraphLinearScan>() &&
      !scan.Is<googlesql::ResolvedGraphTableScan>()) {
    result->ordering.clear();
  }
  return result;
}

std::string StoredColumn(const std::string& column, const googlesql::Type* type,
                         TimestampPrecision precision) {
  if (!HasBigNumeric(type) && !HasGeography(type)) {
    return column;
  }
  return "CAST(" + column + " AS " + DuckDbType(type, precision).value_or("") + ")";
}

}  // namespace bigquery_emulator_duckdb::translator
