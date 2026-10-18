#include "src/parquet_metadata.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <utility>
#include <vector>

#include "src/api_error.h"

namespace bigquery_emulator_duckdb {
namespace {

using Type = ThriftValue::Type;

ThriftValue Binary(std::string bytes) {
  ThriftValue value;
  value.type = Type::kBinary;
  value.bytes = std::move(bytes);
  return value;
}

ThriftValue List(std::vector<ThriftValue> elements) {
  ThriftValue value;
  value.type = Type::kList;
  value.element_type = elements.empty() ? Type::kStruct : elements.front().type;
  value.elements = std::move(elements);
  return value;
}

// A SchemaElement: a group of `children` fields, or a leaf of physical `type` when there are none.
ThriftValue Element(const std::string& name, int64_t repetition, int32_t children,
                    int64_t type = parquet::kInt64) {
  ThriftValue element = ThriftValue::Struct({});
  if (children == 0) {
    element.SetField(parquet::kElementType, ThriftValue::Int32(static_cast<int32_t>(type)));
  }
  element.SetField(parquet::kElementRepetition,
                   ThriftValue::Int32(static_cast<int32_t>(repetition)));
  element.SetField(parquet::kElementName, Binary(name));
  if (children > 0) {
    element.SetField(parquet::kElementChildren, ThriftValue::Int32(children));
  }
  return element;
}

ThriftValue Annotated(ThriftValue element, int64_t converted) {
  element.SetField(parquet::kElementConvertedType,
                   ThriftValue::Int32(static_cast<int32_t>(converted)));
  return element;
}

constexpr int64_t kOptional = 1;

// A Parquet file with an empty footer, which RewriteParquetMetadata then replaces.
std::string EmptyParquetFile() {
  const std::string path =
      (std::filesystem::path(testing::TempDir()) / "parquet_metadata_test.parquet").string();
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << std::string("PAR1\0\x01\0\0\0PAR1", 13);
  return path;
}

TEST(ParquetMetadataTest, RewritesAndReadsBackAFooter) {
  std::vector<ThriftValue> schema = {Element("schema", 0, 18)};
  for (int i = 0; i < 18; ++i) {
    schema.push_back(Element("c" + std::to_string(i), kOptional, 0));
  }
  ThriftValue flag;
  flag.type = Type::kTrue;
  // Field 40 is past the deltas of the short form of a field header, and a list of 19 elements
  // past the short form of a list header.
  ThriftValue metadata = ThriftValue::Struct({
      {.id = 1, .value = ThriftValue::Int32(2)},
      {.id = parquet::kSchema, .value = List(schema)},
      {.id = 9, .value = flag},
      {.id = 40, .value = Binary("x")},
  });
  const std::string path = EmptyParquetFile();
  RewriteParquetMetadata(path, metadata);
  const ThriftValue read = ReadParquetMetadata(path);
  ASSERT_EQ(read.fields.size(), 4);
  EXPECT_EQ(read.Field(1)->integer, 2);
  EXPECT_EQ(read.Field(parquet::kSchema)->elements.size(), 19);
  EXPECT_EQ(read.Field(9)->type, Type::kTrue);
  EXPECT_EQ(read.Field(40)->bytes, "x");
  const ParquetColumn columns = ParquetColumns(read, /*list_inference=*/true);
  ASSERT_EQ(columns.children.size(), 18);
  EXPECT_EQ(columns.children.back().name, "c17");

  // A shorter footer leaves no bytes of the longer one behind.
  metadata.RemoveField(40);
  RewriteParquetMetadata(path, metadata);
  EXPECT_EQ(ReadParquetMetadata(path).Field(40), nullptr);
}

TEST(ParquetMetadataTest, RejectsFilesThatAreNotParquet) {
  const std::string path = EmptyParquetFile();
  std::filesystem::resize_file(path, 11);
  EXPECT_THROW(ReadParquetMetadata(path), ApiError);
}

TEST(ParquetMetadataTest, TellsWideDecimals) {
  ThriftValue wide = Annotated(Element("w", kOptional, 0, parquet::kFixedLenByteArray),
                               parquet::kConvertedDecimal);
  wide.SetField(parquet::kElementScale, ThriftValue::Int32(38));
  wide.SetField(parquet::kElementPrecision, ThriftValue::Int32(76));
  ThriftValue narrow = Annotated(Element("n", kOptional, 0, parquet::kFixedLenByteArray),
                                 parquet::kConvertedDecimal);
  narrow.SetField(parquet::kElementScale, ThriftValue::Int32(9));
  narrow.SetField(parquet::kElementPrecision, ThriftValue::Int32(38));
  const std::vector<ThriftValue> schema = {
      Element("schema", 0, 3), Element("s", kOptional, 1), wide, narrow, Element("i", kOptional, 0),
  };
  const ParquetColumn columns =
      ParquetColumns(ThriftValue::Struct({{.id = parquet::kSchema, .value = List(schema)}}),
                     /*list_inference=*/true);
  EXPECT_TRUE(columns.HasWideDecimal());
  const ParquetColumn& w = columns.children.at(0).children.at(0);
  EXPECT_TRUE(w.IsWideDecimal());
  EXPECT_EQ(w.scale, 38);
  EXPECT_TRUE(columns.children.at(1).IsDecimal());
  EXPECT_FALSE(columns.children.at(1).IsWideDecimal());
  EXPECT_TRUE(columns.children.at(2).IsInteger());
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
