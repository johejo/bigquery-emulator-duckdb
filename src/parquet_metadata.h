#pragma once

// The footer of a Parquet file, its FileMetaData in Thrift's compact protocol, read and rewritten
// in place. DuckDB reads a DECIMAL wider than 38 digits as a DOUBLE and cannot write one, so the
// emulator reads such a column as its bytes and writes BIGNUMERIC as bytes, and edits the
// footer's types to match. See https://github.com/apache/parquet-format.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bigquery_emulator_duckdb {

// A value of Thrift's compact protocol, kept with its type so that it is written back as read.
struct ThriftValue {
  enum class Type : uint8_t {
    kTrue = 1,
    kFalse = 2,
    kByte = 3,
    kI16 = 4,
    kI32 = 5,
    kI64 = 6,
    kDouble = 7,
    kBinary = 8,
    kList = 9,
    kSet = 10,
    kMap = 11,
    kStruct = 12,
  };

  Type type = Type::kStruct;
  // kByte, kI16, kI32 and kI64, and a boolean element of a list, set or map as its byte.
  int64_t integer = 0;
  // kBinary, and kDouble as its eight bytes.
  std::string bytes;
  // kList and kSet: the elements' type and the elements. kMap: the keys' and values' types, and
  // the keys and values alternately.
  Type element_type = Type::kStruct;
  Type value_type = Type::kStruct;
  std::vector<ThriftValue> elements;
  // kStruct: the fields in order of their ids.
  std::vector<std::pair<int16_t, ThriftValue>> fields;

  static ThriftValue Int32(int32_t value);
  static ThriftValue Struct(std::vector<std::pair<int16_t, ThriftValue>> fields);

  // The struct field `id`, or null when it is absent.
  [[nodiscard]] const ThriftValue* Field(int16_t id) const;
  ThriftValue* Field(int16_t id);
  // Sets the struct field `id`, adding it in order when it is absent.
  void SetField(int16_t id, ThriftValue value);
  void RemoveField(int16_t id);
};

// The FileMetaData of the Parquet file `path`. Throws ApiError::Invalid for a file that is not
// Parquet or whose footer is encrypted.
ThriftValue ReadParquetMetadata(const std::string& path);

// Replaces the FileMetaData of the Parquet file `path` with `metadata`, leaving its pages as they
// are.
void RewriteParquetMetadata(const std::string& path, const ThriftValue& metadata);

// The fields of FileMetaData, SchemaElement, LogicalType, DecimalType, RowGroup, ColumnChunk and
// ColumnMetaData that the emulator reads or writes.
namespace parquet {
inline constexpr int16_t kSchema = 2;
inline constexpr int16_t kRowGroups = 4;
inline constexpr int16_t kElementType = 1;
inline constexpr int16_t kElementTypeLength = 2;
inline constexpr int16_t kElementRepetition = 3;
inline constexpr int16_t kElementName = 4;
inline constexpr int16_t kElementChildren = 5;
inline constexpr int16_t kElementConvertedType = 6;
inline constexpr int16_t kElementScale = 7;
inline constexpr int16_t kElementPrecision = 8;
inline constexpr int16_t kElementLogicalType = 10;
inline constexpr int16_t kLogicalMap = 2;
inline constexpr int16_t kLogicalList = 3;
inline constexpr int16_t kLogicalDecimal = 5;
inline constexpr int16_t kLogicalInteger = 10;
inline constexpr int16_t kDecimalScale = 1;
inline constexpr int16_t kDecimalPrecision = 2;
inline constexpr int16_t kRowGroupColumns = 1;
inline constexpr int16_t kChunkMetadata = 3;
inline constexpr int16_t kChunkColumnIndexOffset = 6;
inline constexpr int16_t kChunkColumnIndexLength = 7;
inline constexpr int16_t kColumnStatistics = 12;
inline constexpr int16_t kColumnBloomFilterOffset = 14;
inline constexpr int16_t kColumnBloomFilterLength = 15;

// Values of SchemaElement's type, repetition_type and converted_type.
inline constexpr int64_t kInt32 = 1;
inline constexpr int64_t kInt64 = 2;
inline constexpr int64_t kByteArray = 6;
inline constexpr int64_t kFixedLenByteArray = 7;
inline constexpr int64_t kRepeated = 2;
inline constexpr int64_t kConvertedMap = 1;
inline constexpr int64_t kConvertedMapKeyValue = 2;
inline constexpr int64_t kConvertedList = 3;
inline constexpr int64_t kConvertedDecimal = 5;
// UINT_8 to INT_64.
inline constexpr int64_t kConvertedFirstInteger = 11;
inline constexpr int64_t kConvertedLastInteger = 18;
}  // namespace parquet

// A column of a Parquet file in the shape DuckDB reads it: a group is a STRUCT, and a LIST group
// or a repeated field a LIST.
struct ParquetColumn {
  enum class Kind : uint8_t { kLeaf, kStruct, kList, kMap };

  std::string name;
  Kind kind = Kind::kLeaf;
  // kStruct: the fields. kList: the element, whose name is the list's.
  std::vector<ParquetColumn> children;
  // The index of its SchemaElement in FileMetaData's schema.
  size_t element = 0;
  // kLeaf: its physical type, its ConvertedType or -1, the member of its LogicalType or 0, and
  // a DECIMAL's precision and scale.
  int64_t type = -1;
  int64_t converted = -1;
  int16_t logical = 0;
  int64_t precision = 0;
  int64_t scale = 0;

  bool operator==(const ParquetColumn&) const = default;

  [[nodiscard]] bool IsDecimal() const;
  // A DECIMAL wider than DuckDB's, which it reads as a DOUBLE.
  [[nodiscard]] bool IsWideDecimal() const;
  // An INT32 or INT64 that DuckDB reads as an integer.
  [[nodiscard]] bool IsInteger() const;
  // Whether this column or a descendant is a wide DECIMAL.
  [[nodiscard]] bool HasWideDecimal() const;
  // The field of a kStruct named `field` in any case, or null.
  [[nodiscard]] const ParquetColumn* Child(std::string_view field) const;
};

// The columns of `metadata`, a FileMetaData, as the fields of a STRUCT.
ParquetColumn ParquetColumns(const ThriftValue& metadata);

}  // namespace bigquery_emulator_duckdb
