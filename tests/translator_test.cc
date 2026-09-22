#include "src/translator.h"

#include <string>

#include "gtest/gtest.h"
#include "src/frontend.h"

namespace bigquery_emulator_duckdb {
namespace {

TEST(TranslatorTest, ReturnsSelectOneStub) {
  const FrontendResult frontend_result = ParseGoogleSql("SELECT 1");
  EXPECT_EQ(TranslateToDuckDbSql(frontend_result), "SELECT 1");
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
