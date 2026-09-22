#include "src/backend.h"

#include "gtest/gtest.h"

namespace bigquery_emulator_duckdb {
namespace {

TEST(BackendTest, ExecutesSelectOne) { EXPECT_EQ(ExecuteScalarString("SELECT 1"), "1"); }

}  // namespace
}  // namespace bigquery_emulator_duckdb
