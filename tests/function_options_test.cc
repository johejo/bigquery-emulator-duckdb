#include "src/function_options.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "duckdb.h"
#include "duckdb_static_extension.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "src/duckdb_handle.h"
#include "src/googlesql_functions/format.h"
#include "src/googlesql_functions_duckdb/bignumeric.h"
#include "src/googlesql_functions_duckdb/datetime.h"
#include "src/googlesql_functions_duckdb/json.h"
#include "src/googlesql_functions_duckdb/math.h"
#include "src/googlesql_functions_duckdb/options.h"
#include "src/googlesql_functions_duckdb/string.h"

extern "C" int32_t duckdb_extension_core_functions_describe(duckdb_extension_descriptor*);

namespace bigquery_emulator_duckdb {
namespace {

TEST(FunctionOptionsTest, RegistrationsOwnIndependentPolicies) {
  ASSERT_EQ(duckdb_register_static_extension(duckdb_extension_core_functions_describe), 0);
  std::array<Handle<duckdb_database, duckdb_close>, 3> databases;
  std::array<Connection, 3> connections;
  for (size_t i = 0; i < databases.size(); ++i) {
    ASSERT_EQ(duckdb_open(nullptr, databases.at(i).out()), DuckDBSuccess);
    ASSERT_EQ(duckdb_connect(databases.at(i).get(), connections.at(i).out()), DuckDBSuccess);
    {
      // Destroying the caller's options must not change the registration.
      functions_duckdb::Options options;
      if (i == 1) {
        options = BigQueryFunctionOptions();
      } else if (i == 2) {
        options.chr_zero_is_empty = true;
      }
      functions_duckdb::RegisterStringFunctions(connections.at(i).get(), options);
      functions_duckdb::RegisterDatetimeFunctions(connections.at(i).get(), options);
      functions_duckdb::RegisterMathFunctions(connections.at(i).get(), options);
      functions_duckdb::RegisterBigNumericFunctions(connections.at(i).get(), options);
      functions_duckdb::RegisterJsonFunctions(connections.at(i).get(), options);
    }
  }

  // Alternate connections, including revisiting the first after the others have run.
  for (const size_t i : {0, 1, 2, 0}) {
    duckdb_connection connection = connections.at(i).get();
    const bool bigquery = i == 1;
    struct Case {
      const char* sql;
      bool expected;
    };
    std::tm day{};
    day.tm_year = -1000;
    day.tm_mday = 4;
    day.tm_wday = 1;
    day.tm_yday = 3;
    std::array<char, 32> native_year{};
    ASSERT_NE(std::strftime(native_year.data(), native_year.size(), "%G", &day), 0);
    const std::array cases = {
        Case{.sql = "SELECT length(bq_chr(0)) = 0", .expected = i != 0},
        Case{.sql = "SELECT length(bq_format('%t', chr(0))) = 0", .expected = bigquery},
        Case{
            .sql = "SELECT bq_format_date('%G', DATE '0900-01-04') = '900'",
            .expected = bigquery || std::string_view(native_year.data()) == "900",
        },
        Case{
            .sql = "SELECT bq_json_array_append('[]', '$', struct_pack(value := -0.0::DOUBLE, "
                   "names := '[]'), true) LIKE '%-%'",
            .expected = bigquery,
        },
    };
    for (const auto& test : cases) {
      duckdb_result result{};
      const duckdb_state status = duckdb_query(connection, test.sql, &result);
      EXPECT_EQ(status, DuckDBSuccess) << test.sql << ": " << duckdb_result_error(&result);
      if (status == DuckDBSuccess) {
        EXPECT_FALSE(duckdb_value_is_null(&result, 0, 0)) << test.sql;
        EXPECT_EQ(duckdb_value_boolean(&result, 0, 0), test.expected) << test.sql;
      }
      duckdb_destroy_result(&result);
    }

    // This BIGNUMERIC's TO_JSON conversion loses precision in a DOUBLE; the BigQuery
    // answer is covered by the JSON mutator scalar cases. Native GoogleSQL may round it.
    for (const char* sql : {
             "SELECT bq_json_array_append('[]', '$', struct_pack(value := "
             "'12312345698172318923719827300000000000000'::BIGNUM, names := '[]'), true)",
             "SELECT bq_json_array_insert('[]', '$[0]', struct_pack(value := "
             "'12312345698172318923719827300000000000000'::BIGNUM, names := '[]'), true)",
         }) {
      duckdb_result result{};
      EXPECT_EQ(duckdb_query(connection, sql, &result), bigquery ? DuckDBError : DuckDBSuccess)
          << duckdb_result_error(&result);
      duckdb_destroy_result(&result);
    }

    for (const char* sql : {
             "SELECT len(bq_generate_array(0.0::DOUBLE, 16000.0::DOUBLE, 1.0::DOUBLE))",
             "SELECT len(bq_generate_array_numeric(0::DECIMAL(38,9), 16000::DECIMAL(38,9), "
             "1::DECIMAL(38,9)))",
             "SELECT len(bq_bignumeric_generate_array('0', '16000', '1'))",
             "SELECT len(bq_generate_date_array(DATE '2000-01-01', DATE '2000-01-01' + 16000, 1, "
             "'DAY'))",
         }) {
      duckdb_result result{};
      const auto status = duckdb_query(connection, sql, &result);
      if (bigquery) {
        EXPECT_EQ(status, DuckDBSuccess) << duckdb_result_error(&result);
        if (status == DuckDBSuccess) {
          EXPECT_EQ(duckdb_value_int64(&result, 0, 0), 16001);
        }
      } else {
        EXPECT_EQ(status, DuckDBError) << sql;
      }
      duckdb_destroy_result(&result);
    }
  }
}

// Exercise the semantic library directly to retain the input NaN sign, which SQL cannot write.
// CAST and %t already drop it; adapting floating-point specifiers must preserve negative zero.
TEST(FunctionOptionsTest, NanPolicyPreservesOtherValues) {
  const auto negative_nan =
      googlesql::Value::Double(std::copysign(std::numeric_limits<double>::quiet_NaN(), -1.0));
  const auto array = googlesql::Value::MakeArray(googlesql::types::DoubleArrayType(),
                                                 {negative_nan, googlesql::Value::Double(-0.0)});
  ASSERT_TRUE(array.ok()) << array.status();
  for (const bool unsigned_nan : {false, true}) {
    const googlesql_functions::FormatOptions options{.unsigned_nan = unsigned_nan};
    const auto formatted = googlesql_functions::Format("%t", {*array}, options);
    ASSERT_TRUE(formatted.ok()) << formatted.status();
    ASSERT_TRUE(formatted->has_value());
    EXPECT_EQ((**formatted).find("-nan"), std::string::npos);
    const auto floating = googlesql_functions::Format("%f", {negative_nan}, options);
    ASSERT_TRUE(floating.ok()) << floating.status();
    ASSERT_TRUE(floating->has_value());
    EXPECT_EQ((**floating).find('-') != std::string::npos, !unsigned_nan) << **floating;
    EXPECT_NE((**formatted).find("-0"), std::string::npos);
  }
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
