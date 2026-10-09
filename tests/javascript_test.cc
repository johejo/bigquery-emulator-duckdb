#include "src/backend_functions/javascript.h"

#include <chrono>
#include <cstddef>
#include <string>

#include "duckdb.h"
#include "gtest/gtest.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

// A DuckDB database with the JavaScript backend functions under `limits`.
class JavaScriptTest : public ::testing::Test {
 protected:
  void Open(const JavaScriptLimits& limits) {
    ASSERT_EQ(duckdb_open(nullptr, &database_), DuckDBSuccess);
    ASSERT_EQ(duckdb_connect(database_, &connection_), DuckDBSuccess);
    RegisterJavaScriptFunctions(connection_, limits);
  }

  void TearDown() override {
    duckdb_disconnect(&connection_);
    duckdb_close(&database_);
  }

  // The first value of `sql` as text, or the error prefixed with "error: ".
  std::string Query(const std::string& sql) {
    duckdb_result result;
    if (duckdb_query(connection_, sql.c_str(), &result) == DuckDBError) {
      std::string error = std::string("error: ") + duckdb_result_error(&result);
      duckdb_destroy_result(&result);
      return error;
    }
    char* text = duckdb_value_varchar(&result, 0, 0);
    std::string value = text == nullptr ? "NULL" : text;
    duckdb_free(text);
    duckdb_destroy_result(&result);
    return value;
  }

  duckdb_database database_ = nullptr;
  duckdb_connection connection_ = nullptr;
};

TEST_F(JavaScriptTest, KeepsNoStateAcrossQueries) {
  Open({});
  const std::string call =
      "SELECT bq_js_string('(function() {\n"
      "globalThis.n = (globalThis.n || 0) + 1; return String(globalThis.n);\n})')";
  EXPECT_EQ(Query(call), "1");
  EXPECT_EQ(Query(call), "1");
}

TEST_F(JavaScriptTest, InterruptsACallPastTheTimeLimit) {
  Open({.call_time = std::chrono::milliseconds(100)});
  EXPECT_EQ(Query("SELECT bq_js_double('(function() {\nwhile (true) {}\n})')"),
            "error: Invalid Input Error: JavaScript UDF timed out");
  EXPECT_EQ(Query("SELECT bq_js_double('(function() {\ntry { while (true) {} } catch (e) {} "
                  "return 1;\n})')"),
            "error: Invalid Input Error: JavaScript UDF timed out");
}

TEST_F(JavaScriptTest, FailsACallPastTheMemoryLimit) {
  Open({.memory = size_t{16} << 20U});
  EXPECT_EQ(Query("SELECT bq_js_double('(function() {\n"
                  "const a = []; while (true) a.push(new Array(1000).fill(1));\n})')")
                .rfind("error: ", 0),
            0);
}

TEST_F(JavaScriptTest, RejectsABodyThatClosesTheFunction) {
  Open({});
  EXPECT_EQ(Query("SELECT bq_js_double('(function() {\n}); (function() {\nreturn 1;\n})')"),
            "error: Invalid Input Error: JavaScript UDF body is not a function body");
  EXPECT_EQ(Query("SELECT bq_js_double('(function() {\n} || function() {\nreturn 1;\n})')"),
            "error: Invalid Input Error: JavaScript UDF body is not a function body");
}

}  // namespace
}  // namespace bigquery_emulator_duckdb::backend_functions
