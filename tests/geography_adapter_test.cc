#include <gtest/gtest.h>

#include <array>
#include <cstddef>

#include "duckdb.h"
#include "src/duckdb_handle.h"
#include "src/googlesql_geography/options.h"
#include "src/googlesql_geography_duckdb/geography.h"

namespace bigquery_emulator_duckdb {
namespace {

TEST(GeographyAdapterTest, OwnsOptionsIndependentlyForEachDatabase) {
  Handle<duckdb_database, duckdb_close> first;
  Handle<duckdb_database, duckdb_close> second;
  ASSERT_EQ(duckdb_open(nullptr, first.out()), DuckDBSuccess);
  ASSERT_EQ(duckdb_open(nullptr, second.out()), DuckDBSuccess);
  Connection first_connection;
  Connection second_connection;
  ASSERT_EQ(duckdb_connect(first.get(), first_connection.out()), DuckDBSuccess);
  ASSERT_EQ(duckdb_connect(second.get(), second_connection.out()), DuckDBSuccess);
  {
    googlesql_geography::Options options;
    geography_duckdb::RegisterGeographyFunctions(first_connection.get(), options);
    options.expand_bounding_box = true;
    geography_duckdb::RegisterGeographyFunctions(second_connection.get(), options);
  }

  // Both registrations outlive the original options. Alternating databases also checks
  // that the second registration cannot replace the first database's policy.
  const std::array connections = {
      first_connection.get(),
      second_connection.get(),
      first_connection.get(),
  };
  const std::array expected = {false, true, false};
  for (size_t i = 0; i < connections.size(); ++i) {
    duckdb_result result{};
    const duckdb_state status = duckdb_query(
        connections.at(i), "SELECT (bq_st_boundingbox(bq_st_geogpoint(0, 0))).xmin < 0", &result);
    EXPECT_EQ(status, DuckDBSuccess) << duckdb_result_error(&result);
    if (status == DuckDBSuccess) {
      EXPECT_FALSE(duckdb_value_is_null(&result, 0, 0));
      EXPECT_EQ(duckdb_value_boolean(&result, 0, 0), expected.at(i));
    }
    duckdb_destroy_result(&result);
  }
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
