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
  if (children > 0) element.SetField(parquet::kElementChildren, ThriftValue::Int32(children));
  return element;
}

ThriftValue Annotated(ThriftValue element, int64_t converted) {
  element.SetField(parquet::kElementConvertedType,
                   ThriftValue::Int32(static_cast<int32_t>(converted)));
  return element;
}

constexpr int64_t kOptional = 1;
constexpr int64_t kRepeated = parquet::kRepeated;

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
  for (int i = 0; i < 18; ++i) schema.push_back(Element("c" + std::to_string(i), kOptional, 0));
  ThriftValue flag;
  flag.type = Type::kTrue;
  // Field 40 is past the deltas of the short form of a field header, and a list of 19 elements
  // past the short form of a list header.
  ThriftValue metadata = ThriftValue::Struct({
      {1, ThriftValue::Int32(2)},
      {parquet::kSchema, List(schema)},
      {9, flag},
      {40, Binary("x")},
  });
  const std::string path = EmptyParquetFile();
  RewriteParquetMetadata(path, metadata);
  const ThriftValue read = ReadParquetMetadata(path);
  ASSERT_EQ(read.fields.size(), 4);
  EXPECT_EQ(read.Field(1)->integer, 2);
  EXPECT_EQ(read.Field(parquet::kSchema)->elements.size(), 19);
  EXPECT_EQ(read.Field(9)->type, Type::kTrue);
  EXPECT_EQ(read.Field(40)->bytes, "x");
  const ParquetColumn columns = ParquetColumns(read);
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

// The shapes DuckDB reads, by the format's rules for lists and their backward compatibility.
TEST(ParquetMetadataTest, ColumnsTakeDuckDbsShape) {
  const std::vector<ThriftValue> schema = {
      Element("schema", kRepeated, 6),
      // The standard three-level list.
      Annotated(Element("a", kOptional, 1), parquet::kConvertedList),
      Element("list", kRepeated, 1),
      Element("element", kOptional, 0, parquet::kFixedLenByteArray),
      // A two-level list of a repeated leaf.
      Annotated(Element("b", kOptional, 1), parquet::kConvertedList),
      Element("array", kRepeated, 0),
      // A list whose repeated group named array is a struct of one field.
      Annotated(Element("c", kOptional, 1), parquet::kConvertedList),
      Element("array", kRepeated, 1),
      Element("x", kOptional, 0),
      // A repeated group, unannotated, and a repeated leaf.
      Element("d", kRepeated, 2),
      Element("x", kOptional, 0),
      Element("y", kOptional, 0),
      Element("e", kRepeated, 0),
      // A struct.
      Element("f", kOptional, 1),
      Element("x", kOptional, 0),
  };
  const ParquetColumn columns =
      ParquetColumns(ThriftValue::Struct({{parquet::kSchema, List(schema)}}));
  using Kind = ParquetColumn::Kind;
  ASSERT_EQ(columns.kind, Kind::kStruct);
  ASSERT_EQ(columns.children.size(), 6);

  const ParquetColumn& a = columns.children[0];
  ASSERT_EQ(a.kind, Kind::kList);
  EXPECT_EQ(a.children.at(0).kind, Kind::kLeaf);
  EXPECT_EQ(a.children.at(0).element, 3);
  EXPECT_EQ(a.children.at(0).type, parquet::kFixedLenByteArray);

  const ParquetColumn& b = columns.children[1];
  ASSERT_EQ(b.kind, Kind::kList);
  EXPECT_EQ(b.children.at(0).kind, Kind::kLeaf);

  const ParquetColumn& c = columns.children[2];
  ASSERT_EQ(c.kind, Kind::kList);
  ASSERT_EQ(c.children.at(0).kind, Kind::kStruct);
  EXPECT_NE(c.children.at(0).Child("X"), nullptr);

  const ParquetColumn& d = columns.children[3];
  ASSERT_EQ(d.kind, Kind::kList);
  ASSERT_EQ(d.children.at(0).kind, Kind::kStruct);
  EXPECT_EQ(d.children.at(0).children.size(), 2);

  EXPECT_EQ(columns.children[4].kind, Kind::kList);
  EXPECT_EQ(columns.children[4].children.at(0).kind, Kind::kLeaf);
  EXPECT_EQ(columns.children[5].kind, Kind::kStruct);
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
      ParquetColumns(ThriftValue::Struct({{parquet::kSchema, List(schema)}}));
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
