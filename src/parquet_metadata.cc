#include "src/parquet_metadata.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/api_error.h"

namespace bigquery_emulator_duckdb {
namespace {

using Type = ThriftValue::Type;

constexpr std::string_view kMagic = "PAR1";
constexpr std::string_view kEncryptedMagic = "PARE";
// Deeper nesting than any Parquet footer has, which guards the recursion.
constexpr int kMaxDepth = 64;

[[noreturn]] void Invalid(std::string_view what) {
  throw ApiError::Invalid("Invalid Parquet file: " + std::string(what));
}

class Reader {
 public:
  explicit Reader(std::string_view input) : input_(input) {}

  uint8_t Byte() {
    if (position_ >= input_.size()) Invalid("truncated footer");
    return static_cast<uint8_t>(input_[position_++]);
  }

  uint64_t Varint() {
    uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      const uint8_t byte = Byte();
      value |= static_cast<uint64_t>(byte & 0x7f) << shift;
      if ((byte & 0x80) == 0) return value;
    }
    Invalid("malformed varint");
  }

  int64_t ZigZag() {
    const uint64_t value = Varint();
    return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1);
  }

  std::string Bytes(uint64_t size) {
    if (size > input_.size() - position_) Invalid("truncated footer");
    std::string bytes(input_.substr(position_, size));
    position_ += size;
    return bytes;
  }

  ThriftValue Value(Type type, bool element, int depth) {
    if (depth > kMaxDepth) Invalid("footer nested too deeply");
    ThriftValue value;
    value.type = type;
    switch (type) {
      case Type::kTrue:
      case Type::kFalse:
        // A struct field's boolean is its type; an element's is a byte of its own.
        if (element) value.type = Byte() == 1 ? Type::kTrue : Type::kFalse;
        break;
      case Type::kByte:
        // A byte is signed.
        value.integer = Byte();
        if (value.integer > 127) value.integer -= 256;
        break;
      case Type::kI16:
      case Type::kI32:
      case Type::kI64:
        value.integer = ZigZag();
        break;
      case Type::kDouble:
        value.bytes = Bytes(8);
        break;
      case Type::kBinary:
        value.bytes = Bytes(Varint());
        break;
      case Type::kList:
      case Type::kSet: {
        const uint8_t header = Byte();
        value.element_type = ElementType(header & 0x0f);
        uint64_t size = header >> 4;
        if (size == 15) size = Varint();
        for (uint64_t i = 0; i < size; ++i) {
          value.elements.push_back(Value(value.element_type, true, depth + 1));
        }
        break;
      }
      case Type::kMap: {
        const uint64_t size = Varint();
        if (size == 0) break;
        const uint8_t types = Byte();
        value.element_type = ElementType(types >> 4);
        value.value_type = ElementType(types & 0x0f);
        for (uint64_t i = 0; i < size; ++i) {
          value.elements.push_back(Value(value.element_type, true, depth + 1));
          value.elements.push_back(Value(value.value_type, true, depth + 1));
        }
        break;
      }
      case Type::kStruct: {
        int16_t id = 0;
        for (uint8_t header = Byte(); header != 0; header = Byte()) {
          const uint8_t delta = header >> 4;
          id = static_cast<int16_t>(delta != 0 ? id + delta : ZigZag());
          value.fields.push_back(
              {.id = id, .value = Value(ElementType(header & 0x0f), false, depth + 1)});
        }
        break;
      }
    }
    return value;
  }

 private:
  static Type ElementType(uint8_t type) {
    if (type < static_cast<uint8_t>(Type::kTrue) || type > static_cast<uint8_t>(Type::kStruct)) {
      Invalid("unknown Thrift type");
    }
    return static_cast<Type>(type);
  }

  std::string_view input_;
  size_t position_ = 0;
};

class Writer {
 public:
  [[nodiscard]] const std::string& Output() const { return out_; }

  void Varint(uint64_t value) {
    while (value >= 0x80) {
      out_ += static_cast<char>((value & 0x7f) | 0x80);
      value >>= 7;
    }
    out_ += static_cast<char>(value);
  }

  void ZigZag(int64_t value) {
    const uint64_t shifted = static_cast<uint64_t>(value) << 1;
    Varint(value < 0 ? ~shifted : shifted);
  }

  void Value(const ThriftValue& value, bool element) {
    switch (value.type) {
      case Type::kTrue:
      case Type::kFalse:
        if (element) out_ += static_cast<char>(value.type);
        break;
      case Type::kByte:
        out_ += static_cast<char>(value.integer);
        break;
      case Type::kI16:
      case Type::kI32:
      case Type::kI64:
        ZigZag(value.integer);
        break;
      case Type::kDouble:
        out_ += value.bytes;
        break;
      case Type::kBinary:
        Varint(value.bytes.size());
        out_ += value.bytes;
        break;
      case Type::kList:
      case Type::kSet: {
        const auto type = static_cast<uint8_t>(value.element_type);
        if (value.elements.size() < 15) {
          out_ += static_cast<char>((value.elements.size() << 4) | type);
        } else {
          out_ += static_cast<char>(0xf0 | type);
          Varint(value.elements.size());
        }
        for (const ThriftValue& item : value.elements) Value(item, true);
        break;
      }
      case Type::kMap:
        Varint(value.elements.size() / 2);
        if (value.elements.empty()) break;
        out_ += static_cast<char>((static_cast<uint8_t>(value.element_type) << 4) |
                                  static_cast<uint8_t>(value.value_type));
        for (const ThriftValue& item : value.elements) Value(item, true);
        break;
      case Type::kStruct: {
        int16_t last = 0;
        for (const auto& [id, field] : value.fields) {
          const int delta = id - last;
          const auto type = static_cast<uint8_t>(field.type);
          if (delta > 0 && delta <= 15) {
            out_ += static_cast<char>((delta << 4) | type);
          } else {
            out_ += static_cast<char>(type);
            ZigZag(id);
          }
          last = id;
          Value(field, false);
        }
        out_ += '\0';
        break;
      }
    }
  }

 private:
  std::string out_;
};

// The footer of the Parquet file `path` and the offset it starts at.
std::pair<std::string, uint64_t> ReadFooter(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw ApiError::Invalid("Could not read " + path);
  const uint64_t size = std::filesystem::file_size(path);
  if (size < (2 * kMagic.size()) + 4) Invalid("too short");
  std::string tail(8, '\0');
  input.seekg(static_cast<std::streamoff>(size - tail.size()));
  input.read(tail.data(), static_cast<std::streamsize>(tail.size()));
  if (tail.ends_with(kEncryptedMagic)) {
    throw ApiError::Invalid("The emulator does not support Parquet files with an encrypted footer");
  }
  if (!tail.ends_with(kMagic)) Invalid("no Parquet magic number");
  // The footer's length is little endian, before the magic number.
  const uint64_t length = std::accumulate(
      tail.rbegin() + kMagic.size(), tail.rend(), uint64_t{0},
      [](uint64_t sum, char byte) { return (sum << 8) | static_cast<uint8_t>(byte); });
  if (length > size - tail.size() - kMagic.size()) Invalid("footer length out of range");
  const uint64_t start = size - tail.size() - length;
  std::string footer(length, '\0');
  input.seekg(static_cast<std::streamoff>(start));
  input.read(footer.data(), static_cast<std::streamsize>(length));
  if (!input) throw ApiError::Invalid("Could not read " + path);
  return {std::move(footer), start};
}

const std::vector<ThriftValue>& Schema(const ThriftValue& metadata) {
  const ThriftValue* schema = metadata.Field(parquet::kSchema);
  if (schema == nullptr || schema->elements.empty()) Invalid("no schema");
  return schema->elements;
}

std::optional<int64_t> IntField(const ThriftValue& element, int16_t id) {
  const ThriftValue* field = element.Field(id);
  if (field == nullptr) return std::nullopt;
  return field->integer;
}

// Whether `element`, a SchemaElement, is annotated with the LogicalType union member `member` or
// with one of the ConvertedType values `converted`.
bool Annotated(const ThriftValue& element, int16_t member, const std::vector<int64_t>& converted) {
  const ThriftValue* logical = element.Field(parquet::kElementLogicalType);
  if (logical != nullptr && logical->Field(member) != nullptr) return true;
  return std::ranges::find(converted,
                           IntField(element, parquet::kElementConvertedType).value_or(-1)) !=
         converted.end();
}

// The column of the SchemaElement at `next` and its descendants, which follow it in depth-first
// order, leaving `next` after them. The elements of a LIST group follow DuckDB's reading of the
// format's backward compatibility rules.
ParquetColumn Column(const std::vector<ThriftValue>& schema, size_t& next, int depth) {
  if (depth > kMaxDepth) Invalid("schema nested too deeply");
  if (next >= schema.size()) Invalid("schema has fewer elements than its groups count");
  const ThriftValue& element = schema[next];
  ParquetColumn column;
  column.element = next++;
  if (const ThriftValue* name = element.Field(parquet::kElementName)) column.name = name->bytes;
  const bool repeated =
      IntField(element, parquet::kElementRepetition).value_or(0) == parquet::kRepeated;
  const int64_t count = IntField(element, parquet::kElementChildren).value_or(0);
  if (count == 0) {
    column.type = IntField(element, parquet::kElementType).value_or(-1);
    column.converted = IntField(element, parquet::kElementConvertedType).value_or(-1);
    if (const ThriftValue* logical = element.Field(parquet::kElementLogicalType);
        logical != nullptr && !logical->fields.empty()) {
      column.logical = logical->fields.front().id;
    }
    column.precision = IntField(element, parquet::kElementPrecision).value_or(0);
    column.scale = IntField(element, parquet::kElementScale).value_or(0);
  } else {
    column.kind = ParquetColumn::Kind::kStruct;
    std::vector<size_t> elements;
    for (int64_t i = 0; i < count; ++i) {
      elements.push_back(next);
      column.children.push_back(Column(schema, next, depth + 1));
    }
    if (Annotated(element, parquet::kLogicalMap,
                  {parquet::kConvertedMap, parquet::kConvertedMapKeyValue})) {
      column.kind = ParquetColumn::Kind::kMap;
    } else if (Annotated(element, parquet::kLogicalList, {parquet::kConvertedList})) {
      const ParquetColumn& child = column.children.front();
      if (count != 1 || child.kind != ParquetColumn::Kind::kList ||
          IntField(schema[elements.front()], parquet::kElementRepetition).value_or(0) !=
              parquet::kRepeated) {
        column.kind = ParquetColumn::Kind::kMap;
      } else {
        // A repeated group of one field is that field's list, unless its name marks it as an
        // element struct of one field.
        ParquetColumn item = child.children.front();
        if (item.kind == ParquetColumn::Kind::kStruct && item.children.size() == 1 &&
            child.name != "array" && child.name != column.name + "_tuple") {
          item = item.children.front();
        }
        column.kind = ParquetColumn::Kind::kList;
        column.children = {std::move(item)};
      }
    }
  }
  // Writers differ in the repetition they give the schema's root, which is the row.
  if (!repeated || depth == 0) return column;
  ParquetColumn list;
  list.name = column.name;
  list.kind = ParquetColumn::Kind::kList;
  list.element = column.element;
  list.children.push_back(std::move(column));
  return list;
}

}  // namespace

ThriftValue ThriftValue::Int32(int32_t value) {
  ThriftValue result;
  result.type = Type::kI32;
  result.integer = value;
  return result;
}

ThriftValue ThriftValue::Struct(std::vector<ThriftField> fields) {
  ThriftValue result;
  result.fields = std::move(fields);
  return result;
}

const ThriftValue* ThriftValue::Field(int16_t id) const {
  const auto found = std::ranges::find(fields, id, &ThriftField::id);
  return found == fields.end() ? nullptr : &found->value;
}

ThriftValue* ThriftValue::Field(int16_t id) {
  const auto found = std::ranges::find(fields, id, &ThriftField::id);
  return found == fields.end() ? nullptr : &found->value;
}

void ThriftValue::SetField(int16_t id, ThriftValue value) {
  const auto at = std::ranges::lower_bound(fields, id, {}, &ThriftField::id);
  if (at != fields.end() && at->id == id) {
    at->value = std::move(value);
  } else {
    fields.insert(at, {.id = id, .value = std::move(value)});
  }
}

void ThriftValue::RemoveField(int16_t id) {
  std::erase_if(fields, [id](const auto& field) { return field.id == id; });
}

bool ParquetColumn::IsDecimal() const {
  return kind == Kind::kLeaf &&
         (converted == parquet::kConvertedDecimal || logical == parquet::kLogicalDecimal);
}

bool ParquetColumn::IsWideDecimal() const { return IsDecimal() && precision > 38; }

bool ParquetColumn::IsInteger() const {
  return kind == Kind::kLeaf && (type == parquet::kInt32 || type == parquet::kInt64) &&
         (converted == -1 || (converted >= parquet::kConvertedFirstInteger &&
                              converted <= parquet::kConvertedLastInteger)) &&
         (logical == 0 || logical == parquet::kLogicalInteger);
}

bool ParquetColumn::HasWideDecimal() const {
  return IsWideDecimal() || std::ranges::any_of(children, [](const ParquetColumn& child) {
           return child.HasWideDecimal();
         });
}

const ParquetColumn* ParquetColumn::Child(std::string_view field) const {
  if (kind != Kind::kStruct) return nullptr;
  const auto found = std::ranges::find_if(children, [field](const ParquetColumn& child) {
    return std::ranges::equal(child.name, field, [](unsigned char a, unsigned char b) {
      return std::tolower(a) == std::tolower(b);
    });
  });
  return found == children.end() ? nullptr : &*found;
}

ThriftValue ReadParquetMetadata(const std::string& path) {
  const auto [footer, start] = ReadFooter(path);
  Reader reader(footer);
  return reader.Value(Type::kStruct, false, 0);
}

void RewriteParquetMetadata(const std::string& path, const ThriftValue& metadata) {
  const uint64_t start = ReadFooter(path).second;
  Writer writer;
  writer.Value(metadata, false);
  std::string footer = writer.Output();
  const uint64_t length = footer.size();
  for (int i = 0; i < 4; ++i) footer += static_cast<char>((length >> (8 * i)) & 0xff);
  footer += kMagic;
  {
    std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
    output.seekp(static_cast<std::streamoff>(start));
    output.write(footer.data(), static_cast<std::streamsize>(footer.size()));
    if (!output) throw ApiError::Invalid("Could not write " + path);
  }
  std::filesystem::resize_file(path, start + footer.size());
}

ParquetColumn ParquetColumns(const ThriftValue& metadata) {
  const std::vector<ThriftValue>& schema = Schema(metadata);
  size_t next = 0;
  ParquetColumn root = Column(schema, next, 0);
  if (root.kind != ParquetColumn::Kind::kStruct) Invalid("schema root is not a group");
  return root;
}

}  // namespace bigquery_emulator_duckdb
