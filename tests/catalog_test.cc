#include "src/catalog.h"

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "gmock/gmock.h"
#include "googlesql/public/type.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "gtest/gtest.h"
#include "src/analyzer.h"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {
namespace {

using Path = std::vector<std::string>;

class FakeTableSource : public TableSource {
 public:
  void Add(const std::string& project, const std::string& dataset, const std::string& table,
           std::vector<FieldSchema> schema) {
    tables_[{project, dataset, table}] = std::move(schema);
  }

  std::optional<std::vector<FieldSchema>> FindTable(const std::string& project,
                                                    const std::string& dataset,
                                                    const std::string& table) override {
    ++lookups;
    auto it = tables_.find({project, dataset, table});
    if (it == tables_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  int lookups = 0;

 private:
  std::map<std::tuple<std::string, std::string, std::string>, std::vector<FieldSchema>> tables_;
};

std::string TypeName(const FieldSchema& field) {
  googlesql::TypeFactory type_factory;
  absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(field, &type_factory);
  if (!type.ok()) {
    return type.status().ToString();
  }
  return (*type)->ShortTypeName(googlesql::PRODUCT_EXTERNAL);
}

// A field as "name TYPE MODE", followed by its child fields in parentheses for a RECORD.
std::string Describe(const FieldSchema& field) {
  std::string description = field.name + " " + field.type + " " + field.mode;
  if (!field.fields.empty()) {
    description += " (";
    for (size_t i = 0; i < field.fields.size(); ++i) {
      description += (i == 0 ? "" : ", ") + Describe(field.fields[i]);
    }
    description += ")";
  }
  return description;
}

std::string BigQueryTypeOf(const googlesql::Type* type) {
  absl::StatusOr<FieldSchema> field = BigQueryFieldSchema("c", type);
  return field.ok() ? Describe(*field) : field.status().ToString();
}

TEST(NormalizeTablePathTest, CompletesMissingProjectAndDataset) {
  EXPECT_EQ(NormalizeTablePath(Path{"t"}, "p", "d"), (Path{"p", "d", "t"}));
  EXPECT_EQ(NormalizeTablePath(Path{"ds", "t"}, "p", "d"), (Path{"p", "ds", "t"}));
  EXPECT_EQ(NormalizeTablePath(Path{"pr", "ds", "t"}, "p", "d"), (Path{"pr", "ds", "t"}));
}

TEST(NormalizeTablePathTest, SplitsElementsWithDots) {
  EXPECT_EQ(NormalizeTablePath(Path{"pr.ds.t"}, "p", "d"), (Path{"pr", "ds", "t"}));
  EXPECT_EQ(NormalizeTablePath(Path{"ds.t"}, "p", "d"), (Path{"p", "ds", "t"}));
  EXPECT_EQ(NormalizeTablePath(Path{"pr.ds", "t"}, "p", "d"), (Path{"pr", "ds", "t"}));
}

TEST(NormalizeTablePathTest, KeepsDomainScopedProjectTogether) {
  const Path expected{"example.com:proj", "ds", "t"};
  EXPECT_EQ(NormalizeTablePath(Path{"example.com:proj.ds.t"}, "p", "d"), expected);
  EXPECT_EQ(NormalizeTablePath(Path{"example.com:proj", "ds", "t"}, "p", "d"), expected);
  EXPECT_EQ(NormalizeTablePath(Path{"example", "com:proj", "ds", "t"}, "p", "d"), expected);
  EXPECT_EQ(NormalizeTablePath(Path{"t"}, "example.com:proj", "ds"), expected);
}

TEST(NormalizeTablePathTest, RejectsWhatItCannotInterpret) {
  EXPECT_TRUE(NormalizeTablePath(Path{"a", "b", "c", "d"}, "p", "d").empty());
  EXPECT_TRUE(NormalizeTablePath(Path{"a.b.c.d"}, "p", "d").empty());
  EXPECT_TRUE(NormalizeTablePath(Path{"t"}, "p", "").empty());
  EXPECT_TRUE(NormalizeTablePath(Path{"ds..t"}, "p", "d").empty());
}

TEST(GoogleSqlTypeTest, MapsScalarTypes) {
  EXPECT_EQ(TypeName({.name = "c", .type = "INTEGER"}), "INT64");
  EXPECT_EQ(TypeName({.name = "c", .type = "INT64"}), "INT64");
  EXPECT_EQ(TypeName({.name = "c", .type = "FLOAT"}), "FLOAT64");
  EXPECT_EQ(TypeName({.name = "c", .type = "BOOLEAN"}), "BOOL");
  EXPECT_EQ(TypeName({.name = "c", .type = "STRING"}), "STRING");
  EXPECT_EQ(TypeName({.name = "c", .type = "BYTES"}), "BYTES");
  EXPECT_EQ(TypeName({.name = "c", .type = "DATE"}), "DATE");
  EXPECT_EQ(TypeName({.name = "c", .type = "TIME"}), "TIME");
  EXPECT_EQ(TypeName({.name = "c", .type = "DATETIME"}), "DATETIME");
  EXPECT_EQ(TypeName({.name = "c", .type = "TIMESTAMP"}), "TIMESTAMP");
  EXPECT_EQ(TypeName({.name = "c", .type = "NUMERIC"}), "NUMERIC");
  EXPECT_EQ(TypeName({.name = "c", .type = "BIGNUMERIC"}), "BIGNUMERIC");
  EXPECT_EQ(TypeName({.name = "c", .type = "JSON"}), "JSON");
  EXPECT_EQ(TypeName({.name = "c", .type = "INTERVAL"}), "INTERVAL");
  EXPECT_EQ(TypeName({.name = "c", .type = "GEOGRAPHY"}), "GEOGRAPHY");
}

TEST(GoogleSqlTypeTest, MapsRepeatedAndRecordFields) {
  EXPECT_EQ(TypeName({.name = "c", .type = "STRING", .mode = "REPEATED"}), "ARRAY<STRING>");
  EXPECT_EQ(TypeName({.name = "c",
                      .type = "RECORD",
                      .fields = {{.name = "a", .type = "INTEGER"},
                                 {.name = "b", .type = "STRING", .mode = "REPEATED"}}}),
            "STRUCT<a INT64, b ARRAY<STRING>>");
  EXPECT_EQ(TypeName({.name = "c",
                      .type = "RECORD",
                      .mode = "REPEATED",
                      .fields = {{.name = "a",
                                  .type = "RECORD",
                                  .mode = "REPEATED",
                                  .fields = {{.name = "x", .type = "FLOAT"}}}}}),
            "ARRAY<STRUCT<a ARRAY<STRUCT<x FLOAT64>>>>");
}

TEST(GoogleSqlTypeTest, RejectsUnknownTypes) {
  googlesql::TypeFactory type_factory;
  EXPECT_FALSE(GoogleSqlType({.name = "c", .type = "NOPE"}, &type_factory).ok());
}

TEST(BigQueryFieldSchemaTest, MapsScalarTypes) {
  EXPECT_EQ(BigQueryTypeOf(googlesql::types::Int64Type()), "c INTEGER NULLABLE");
  EXPECT_EQ(BigQueryTypeOf(googlesql::types::DoubleType()), "c FLOAT NULLABLE");
  EXPECT_EQ(BigQueryTypeOf(googlesql::types::BoolType()), "c BOOLEAN NULLABLE");
  EXPECT_EQ(BigQueryTypeOf(googlesql::types::StringType()), "c STRING NULLABLE");
  EXPECT_EQ(BigQueryTypeOf(googlesql::types::TimestampType()), "c TIMESTAMP NULLABLE");
  EXPECT_EQ(BigQueryTypeOf(googlesql::types::BigNumericType()), "c BIGNUMERIC NULLABLE");
}

TEST(BigQueryFieldSchemaTest, RoundTripsGoogleSqlType) {
  googlesql::TypeFactory type_factory;
  const FieldSchema field = {.name = "c",
                             .type = "RECORD",
                             .mode = "REPEATED",
                             .fields = {{.name = "x", .type = "DATE", .mode = "NULLABLE"},
                                        {.name = "y", .type = "STRING", .mode = "REPEATED"}}};
  absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(field, &type_factory);
  ASSERT_TRUE(type.ok()) << type.status();
  EXPECT_EQ(BigQueryTypeOf(*type), Describe(field));
}

TEST(BigQueryFieldSchemaTest, RejectsTypesBigQueryCannotDescribe) {
  EXPECT_THAT(BigQueryTypeOf(googlesql::types::Int32Type()), ::testing::HasSubstr("INT32"));
}

std::string ColumnType(const FieldSchema& field) {
  absl::StatusOr<std::string> type = DuckDbColumnType(field);
  return type.ok() ? *type : std::string(type.status().message());
}

TEST(DuckDbColumnTypeTest, MapsFieldsThroughTheirGoogleSqlTypes) {
  EXPECT_EQ(ColumnType({.name = "c", .type = "INTEGER"}), "BIGINT");
  EXPECT_EQ(ColumnType({.name = "c", .type = "TIMESTAMP"}), "TIMESTAMPTZ");
  EXPECT_EQ(ColumnType({.name = "c", .type = "DATETIME"}), "TIMESTAMP");
  EXPECT_EQ(ColumnType({.name = "c", .type = "NUMERIC"}), "DECIMAL(38,9)");
  EXPECT_EQ(ColumnType({.name = "c",
                        .type = "RECORD",
                        .mode = "REPEATED",
                        .fields = {{.name = "a", .type = "STRING", .mode = "REPEATED"}}}),
            "STRUCT(\"a\" VARCHAR[])[]");
}

// Tables keep GEOGRAPHY as text, though queries over it are not translated.
TEST(DuckDbColumnTypeTest, StoresGeographyAsText) {
  EXPECT_EQ(ColumnType({.name = "c", .type = "GEOGRAPHY", .mode = "REPEATED"}), "VARCHAR[]");
  EXPECT_EQ(DuckDbType(googlesql::types::GeographyType()), std::nullopt);
}

TEST(DuckDbColumnTypeTest, RejectsTypesDuckDbCannotStore) {
  EXPECT_THAT(ColumnType({.name = "c", .type = "INTERVAL"}), ::testing::HasSubstr("INTERVAL"));
  EXPECT_THAT(
      ColumnType({.name = "c",
                  .type = "RECORD",
                  .fields = {{.name = "a", .type = "STRING"}, {.name = "A", .type = "INTEGER"}}}),
      ::testing::HasSubstr("Unsupported field type"));
}

class BigQueryCatalogTest : public ::testing::Test {
 protected:
  BigQueryCatalogTest() {
    source_.Add("p", "ds", "t",
                {{.name = "a", .type = "INTEGER", .mode = "NULLABLE"},
                 {.name = "b", .type = "STRING", .mode = "REPEATED"}});
  }

  AnalyzerResult Analyze(const std::string& sql, const AnalyzerSettings& settings = {}) {
    return AnalyzeGoogleSql(sql, catalog_, type_factory_, settings);
  }

  // The names and types of the output columns of a query.
  std::vector<std::string> OutputColumns(const std::string& sql,
                                         const AnalyzerSettings& settings = {}) {
    const AnalyzerResult result = Analyze(sql, settings);
    const auto* query = result.statement().GetAs<googlesql::ResolvedQueryStmt>();
    std::vector<std::string> columns;
    for (const auto& column : query->output_column_list()) {
      columns.push_back(column->name() + " " +
                        column->column().type()->ShortTypeName(googlesql::PRODUCT_EXTERNAL));
    }
    return columns;
  }

  FakeTableSource source_;
  googlesql::TypeFactory type_factory_;
  BigQueryCatalog catalog_{source_, &type_factory_, "p", "ds"};
};

TEST_F(BigQueryCatalogTest, FindsTablesThroughTheSource) {
  const googlesql::Table* table = nullptr;
  ASSERT_TRUE(catalog_.FindTable(Path{"ds", "t"}, &table).ok());
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->Name(), "t");
  EXPECT_EQ(table->FullName(), "p.ds.t");
  ASSERT_EQ(table->NumColumns(), 2);
  EXPECT_EQ(table->GetColumn(0)->Name(), "a");
  EXPECT_TRUE(table->GetColumn(0)->GetType()->IsInt64());
  EXPECT_EQ(table->GetColumn(1)->Name(), "b");
  EXPECT_EQ(table->GetColumn(1)->GetType()->ShortTypeName(googlesql::PRODUCT_EXTERNAL),
            "ARRAY<STRING>");

  // The same table is served from the catalog on later lookups, however it is spelled.
  const googlesql::Table* again = nullptr;
  ASSERT_TRUE(catalog_.FindTable(Path{"p.ds.t"}, &again).ok());
  EXPECT_EQ(again, table);
  EXPECT_EQ(source_.lookups, 1);
}

TEST_F(BigQueryCatalogTest, ReportsMissingTablesAsNotFound) {
  const googlesql::Table* table = nullptr;
  EXPECT_TRUE(absl::IsNotFound(catalog_.FindTable(Path{"ds", "missing"}, &table)));
  EXPECT_EQ(table, nullptr);
  EXPECT_TRUE(absl::IsNotFound(catalog_.FindTable(Path{"a", "b", "c", "d"}, &table)));
}

TEST_F(BigQueryCatalogTest, ResolvesColumnsAndTheirTypes) {
  EXPECT_EQ(OutputColumns("SELECT a FROM ds.t"), (std::vector<std::string>{"a INT64"}));
  EXPECT_EQ(OutputColumns("SELECT a + 1 AS x, b FROM `p.ds.t`"),
            (std::vector<std::string>{"x INT64", "b ARRAY<STRING>"}));
  EXPECT_EQ(OutputColumns("SELECT a FROM t"), (std::vector<std::string>{"a INT64"}));
}

TEST_F(BigQueryCatalogTest, ExpandsSelectStar) {
  EXPECT_EQ(OutputColumns("SELECT * FROM ds.t"),
            (std::vector<std::string>{"a INT64", "b ARRAY<STRING>"}));
}

TEST_F(BigQueryCatalogTest, ResolvesBuiltinFunctions) {
  EXPECT_EQ(OutputColumns("SELECT BYTE_LENGTH(CAST(a AS STRING)) AS n FROM ds.t"),
            (std::vector<std::string>{"n INT64"}));
}

TEST_F(BigQueryCatalogTest, TypesQueryParameters) {
  AnalyzerSettings named;
  named.named_parameters = {{"x", googlesql::types::StringType()}};
  EXPECT_EQ(OutputColumns("SELECT @x AS x", named), (std::vector<std::string>{"x STRING"}));

  AnalyzerSettings positional;
  positional.positional_parameters = {googlesql::types::DoubleType()};
  EXPECT_EQ(OutputColumns("SELECT ? AS x", positional), (std::vector<std::string>{"x FLOAT64"}));
}

TEST_F(BigQueryCatalogTest, RejectsUnknownTables) {
  try {
    Analyze("SELECT a FROM ds.missing");
    FAIL() << "expected an analysis error";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("ds.missing"), std::string::npos) << e.what();
  }
}

TEST_F(BigQueryCatalogTest, RejectsUnknownColumns) {
  try {
    Analyze("SELECT nope FROM ds.t");
    FAIL() << "expected an analysis error";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("Unrecognized name: nope"), std::string::npos) << e.what();
  }
}

TEST_F(BigQueryCatalogTest, ResolvesBigQueryOnlyFunctions) {
  EXPECT_EQ(OutputColumns("SELECT CONTAINS_SUBSTR('abc', 'b') AS c"),
            (std::vector<std::string>{"c BOOL"}));
}

TEST_F(BigQueryCatalogTest, DescribesTheResultSchema) {
  const std::optional<std::vector<FieldSchema>> result_schema =
      Analyze("SELECT a, a + 1, b, STRUCT(1 AS x, 'y'), 1.5 FROM ds.t").result_schema();
  if (!result_schema.has_value()) {
    FAIL() << "the query has no result schema";
  }
  const std::vector<FieldSchema>& schema = *result_schema;
  ASSERT_EQ(schema.size(), 5);
  EXPECT_EQ(Describe(schema[0]), "a INTEGER NULLABLE");
  EXPECT_EQ(Describe(schema[1]), "f0_ INTEGER NULLABLE");
  EXPECT_EQ(Describe(schema[2]), "b STRING REPEATED");
  EXPECT_EQ(Describe(schema[3]),
            "f1_ RECORD NULLABLE (x INTEGER NULLABLE, _field_2 STRING NULLABLE)");
  EXPECT_EQ(Describe(schema[4]), "f2_ FLOAT NULLABLE");
}

TEST_F(BigQueryCatalogTest, HasNoResultSchemaForDml) {
  EXPECT_FALSE(Analyze("INSERT INTO ds.t (a) VALUES (1)").result_schema().has_value());
}

TEST_F(BigQueryCatalogTest, AnalyzesDdlAndDml) {
  EXPECT_NO_THROW(Analyze("INSERT INTO ds.t (a) VALUES (1)"));
  EXPECT_NO_THROW(Analyze("CREATE TABLE ds.u (x INT64)"));
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
