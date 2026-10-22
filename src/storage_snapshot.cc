#include "src/storage_snapshot.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "duckdb.h"
#include "googlesql/public/types/timestamp_util.h"
#include "nanoarrow/nanoarrow.h"
#include "nanoarrow/nanoarrow.hpp"
#include "src/api_error.h"
#include "src/backend_error.h"
#include "src/bignumeric.h"
#include "src/duckdb_handle.h"
#include "src/field_schema.h"
#include "src/result_chunks.h"
#include "src/storage_read_codec.h"
#include "src/vector_cell.h"

namespace bigquery_emulator_duckdb {
namespace {

void Check(int code, const ArrowError& error) {
  if (code != NANOARROW_OK) {
    throw BackendError("Arrow IPC operation failed: " + std::string(error.message));
  }
}

void Check(duckdb_error_data error) {
  const Handle<duckdb_error_data, duckdb_destroy_error_data> owned(error);
  if (error != nullptr && duckdb_error_data_has_error(error)) {
    throw BackendError(duckdb_error_data_message(error));
  }
}

void DestroySelection(duckdb_selection_vector* selection) {
  if (*selection != nullptr) {
    duckdb_destroy_selection_vector(*selection);
  }
}

std::string_view Format(FieldType type) {
  switch (type) {
    case FieldType::kInteger:
      return "l";
    case FieldType::kFloat:
      return "g";
    case FieldType::kBoolean:
      return "b";
    case FieldType::kBytes:
      return "z";
    case FieldType::kString:
      return "u";
    case FieldType::kDate:
      return "tdD";
    case FieldType::kTime:
      return "ttu";
    case FieldType::kDatetime:
      return "tsu:";
    case FieldType::kTimestamp:
      return "tsu:UTC";
    case FieldType::kNumeric:
      return "d:38,9,128";
    case FieldType::kBigNumeric:
      // DuckDB exports the stored bytes of a BIGNUM, which Normalize decodes.
      return "z";
    case FieldType::kRecord:
    case FieldType::kRange:
      return "+s";
    default:
      return {};
  }
}

bool Eligible(const FieldSchema& field) {
  return !Format(field.type).empty() && std::ranges::all_of(field.fields, Eligible);
}

// The element of the REPEATED `field`, as Storage Read gives it.
FieldSchema Item(const FieldSchema& field) {
  FieldSchema item = field;
  item.mode = FieldMode::kRequired;
  return item;
}

bool NativeStorage(duckdb_logical_type type) {
  // Arrow export can also convert values, such as discarding a TIME_TZ offset. Restrict the
  // direct path to the physical types used for BigQuery storage, then check their Arrow layout.
  switch (duckdb_get_type_id(type)) {
    case DUCKDB_TYPE_BOOLEAN:
    case DUCKDB_TYPE_BIGINT:
    case DUCKDB_TYPE_DOUBLE:
    case DUCKDB_TYPE_VARCHAR:
    case DUCKDB_TYPE_BLOB:
    case DUCKDB_TYPE_DATE:
    case DUCKDB_TYPE_TIME:
    case DUCKDB_TYPE_TIMESTAMP:
    case DUCKDB_TYPE_TIMESTAMP_TZ:
    case DUCKDB_TYPE_DECIMAL:
    // Query results keep a BIGNUMERIC as its units of 10^-38, with or without the storage alias.
    case DUCKDB_TYPE_BIGNUM:
      return true;
    case DUCKDB_TYPE_LIST: {
      const LogicalType child(duckdb_list_type_child_type(type));
      return NativeStorage(child.get());
    }
    case DUCKDB_TYPE_STRUCT:
      for (idx_t i = 0; i < duckdb_struct_type_child_count(type); ++i) {
        const LogicalType child(duckdb_struct_type_child_type(type, i));
        if (!NativeStorage(child.get())) {
          return false;
        }
      }
      return true;
    default:
      return false;
  }
}

bool Matches(const ArrowSchema& arrow, const FieldSchema& field) {
  if (field.mode == FieldMode::kRepeated) {
    return std::string_view(arrow.format) == "+l" && arrow.n_children == 1 &&
           Matches(*arrow.children[0], Item(field));
  }
  if (field.type == FieldType::kRange) {
    return std::string_view(arrow.format) == "+s" && arrow.n_children == 2 &&
           Matches(*arrow.children[0], RangeBoundField(field, "start")) &&
           Matches(*arrow.children[1], RangeBoundField(field, "end"));
  }
  if (std::string_view(arrow.format) != Format(field.type) ||
      std::cmp_not_equal(arrow.n_children, field.fields.size())) {
    return false;
  }
  for (size_t i = 0; i < field.fields.size(); ++i) {
    if (!Matches(*arrow.children[i], field.fields.at(i))) {
      return false;
    }
  }
  return true;
}

// Rewrites a DuckDB Arrow export, whose layout Matches `field`, into the type `wire` that
// Storage Read gives it. A NULL array, whose DuckDB offsets have zero length, becomes an empty
// one, a BIGNUM's stored bytes become its units of 10^-38, a decimal256 integer, and infinite
// RANGE bounds become NULL.
void Normalize(ArrowArray* array, const ArrowSchema& wire, const FieldSchema& field) {
  if (field.mode == FieldMode::kRepeated) {
    array->buffers[0] = nullptr;
    array->null_count = 0;
    Normalize(array->children[0], *wire.children[0], Item(field));
    return;
  }
  if (field.type == FieldType::kRecord) {
    for (size_t i = 0; i < field.fields.size(); ++i) {
      Normalize(array->children[i], *wire.children[i], field.fields.at(i));
    }
    return;
  }
  if (field.type == FieldType::kRange) {
    // DuckDB stores unbounded ends as temporal infinities; Storage Read uses NULL.
    for (int64_t i = 0; i < 2; ++i) {
      ArrowArray* bound = array->children[i];
      ArrowError error{};
      nanoarrow::UniqueArrayView stored;
      Check(ArrowArrayViewInitFromSchema(stored.get(), wire.children[i], &error), error);
      Check(ArrowArrayViewSetArray(stored.get(), bound, &error), error);
      nanoarrow::UniqueArray normalized;
      Check(ArrowArrayInitFromSchema(normalized.get(), wire.children[i], &error), error);
      Check(ArrowArrayStartAppending(normalized.get()), error);
      Check(ArrowArrayReserve(normalized.get(), bound->length), error);
      for (int64_t row = 0; row < bound->length; ++row) {
        if (ArrowArrayViewIsNull(stored.get(), row) != 0) {
          Check(ArrowArrayAppendNull(normalized.get(), 1), error);
          continue;
        }
        const int64_t value = ArrowArrayViewGetIntUnsafe(stored.get(), row);
        const bool finite = field.range_element_type == FieldType::kDate
                                ? duckdb_is_finite_date({static_cast<int32_t>(value)})
                                : duckdb_is_finite_timestamp({value});
        if (!finite) {
          Check(ArrowArrayAppendNull(normalized.get(), 1), error);
        } else {
          Check(ArrowArrayAppendInt(normalized.get(), value), error);
        }
      }
      Check(ArrowArrayFinishBuildingDefault(normalized.get(), &error), error);
      ArrowArrayRelease(bound);
      ArrowArrayMove(normalized.get(), bound);
    }
    return;
  }
  if (field.type != FieldType::kBigNumeric) {
    return;
  }
  ArrowError error{};
  nanoarrow::UniqueArrayView stored;
  ArrowArrayViewInitFromType(stored.get(), NANOARROW_TYPE_BINARY);
  Check(ArrowArrayViewSetArray(stored.get(), array, &error), error);
  nanoarrow::UniqueArray decimals;
  Check(ArrowArrayInitFromSchema(decimals.get(), &wire, &error), error);
  Check(ArrowArrayStartAppending(decimals.get()), error);
  Check(ArrowArrayReserve(decimals.get(), array->length), error);
  ArrowDecimal decimal{};
  ArrowDecimalInit(&decimal, 256, 76, 38);
  for (int64_t row = 0; row < array->length; ++row) {
    if (ArrowArrayViewIsNull(stored.get(), row) != 0) {
      Check(ArrowArrayAppendNull(decimals.get(), 1), error);
      continue;
    }
    const ArrowBufferView bytes = ArrowArrayViewGetBytesUnsafe(stored.get(), row);
    const auto number = BigNumericFromBignum(
        std::string_view(bytes.data.as_char,  // NOLINT(cppcoreguidelines-pro-type-union-access)
                         static_cast<size_t>(bytes.size_bytes)));
    if (!number.ok()) {
      throw BackendError("DuckDB returned an invalid BIGNUMERIC: " +
                         std::string(number.status().message()));
    }
    // GoogleSQL's packed form is little-endian two's complement; sign extend it to 32 bytes.
    std::string packed = number->SerializeAsProtoBytes();
    const bool negative =
        !packed.empty() && (static_cast<unsigned char>(packed.back()) & 0x80U) != 0;
    packed.resize(32, negative ? static_cast<char>(0xff) : '\0');
    ArrowDecimalSetBytes(&decimal, reinterpret_cast<const uint8_t*>(packed.data()));
    Check(ArrowArrayAppendDecimal(decimals.get(), &decimal), error);
  }
  Check(ArrowArrayFinishBuildingDefault(decimals.get(), &error), error);
  ArrowArrayRelease(array);
  ArrowArrayMove(decimals.get(), array);
}

// Vector conversion parses civil times with GoogleSQL. Keep those range checks when their physical
// Arrow buffers are passed through, including for externally created tables.
void ValidateCivilTime(const ArrowArrayView& view, const FieldSchema& field, int64_t row) {
  if (ArrowArrayViewIsNull(&view, row) != 0) {
    return;
  }
  if (field.mode == FieldMode::kRepeated) {
    const FieldSchema item = Item(field);
    for (int64_t i = ArrowArrayViewListChildOffset(&view, row);
         i < ArrowArrayViewListChildOffset(&view, row + 1); ++i) {
      ValidateCivilTime(*view.children[0], item, i);
    }
    return;
  }
  if (field.type == FieldType::kRecord) {
    for (size_t i = 0; i < field.fields.size(); ++i) {
      ValidateCivilTime(*view.children[i], field.fields.at(i), row);
    }
    return;
  }
  if (field.type == FieldType::kRange) {
    for (int64_t i = 0; i < 2; ++i) {
      ValidateCivilTime(*view.children[i], RangeBoundField(field, ""), row);
    }
    return;
  }
  int64_t min = 0;
  int64_t max = 0;
  switch (field.type) {
    case FieldType::kDate:
      min = googlesql::types::kDateMin;
      max = googlesql::types::kDateMax;
      break;
    case FieldType::kDatetime:
      min = googlesql::types::kTimestampMin;
      max = googlesql::types::kTimestampMax;
      break;
    case FieldType::kTime:
      max = (86400LL * 1000000) - 1;
      break;
    default:
      return;
  }
  const int64_t value = ArrowArrayViewGetIntUnsafe(&view, row);
  if (value < min || value > max) {
    throw ApiError::Invalid("Invalid " + std::string(FieldTypeName(field.type)) + " value");
  }
}

}  // namespace

class StorageSnapshot::Impl {
 public:
  // Destroy options and chunks before the session, whose connection their properties name.
  std::shared_ptr<Backend> session;
  Handle<duckdb_arrow_options, duckdb_destroy_arrow_options> options;
  std::vector<FieldSchema> fields;
  // Whether DuckDB exports each column in the layout Storage Read gives it.
  std::vector<bool> direct;
  std::vector<duckdb_logical_type> direct_types;
  nanoarrow::UniqueSchema wire;
  std::unique_ptr<const ResultChunks> chunks;
};

StorageSnapshot::StorageSnapshot(duckdb_result& result, std::vector<FieldSchema> schema,
                                 std::shared_ptr<Backend> session)
    : impl_(std::make_unique<Impl>()) {
  auto& state = *impl_;
  if (schema.size() != duckdb_column_count(&result)) {
    throw ApiError::Invalid("Table changed while creating the Storage Read snapshot");
  }
  state.session = std::move(session);
  state.fields = std::move(schema);
  *state.options.out() = duckdb_result_get_arrow_options(&result);
  StorageReadArraySchema(state.wire.get(), state.fields, false);
  state.chunks = std::make_unique<const ResultChunks>(result, state.session);
  for (idx_t i = 0; i < state.fields.size(); ++i) {
    duckdb_logical_type type = state.chunks->type(i);
    bool direct = Eligible(state.fields.at(i)) && NativeStorage(type);
    if (direct) {
      // Some DuckDB types have no Arrow representation; their export fails and they use vector
      // conversion.
      const char* name = duckdb_column_name(&result, i);
      nanoarrow::UniqueSchema exported;
      const Handle<duckdb_error_data, duckdb_destroy_error_data> error(
          duckdb_to_arrow_schema(state.options.get(), &type, &name, 1, exported.get()));
      direct = (error.get() == nullptr || !duckdb_error_data_has_error(error.get())) &&
               Matches(*exported->children[0], state.fields.at(i));
    }
    state.direct.push_back(direct);
    if (direct) {
      state.direct_types.push_back(type);
    }
  }
}

StorageSnapshot::~StorageSnapshot() = default;

const std::vector<FieldSchema>& StorageSnapshot::schema() const { return impl_->fields; }

size_t StorageSnapshot::row_count() const { return impl_->chunks->row_count(); }

std::string StorageSnapshot::ReadRows(size_t offset, size_t count, bool arrow) const {
  auto& state = *impl_;
  if (offset > row_count() || count > row_count() - offset || count > duckdb_vector_size()) {
    throw std::out_of_range("Storage Read row range is outside the snapshot or vector capacity");
  }
  nanoarrow::UniqueArray root;
  ArrowError error{};
  Check(ArrowArrayInitFromSchema(root.get(), state.wire.get(), &error), error);
  for (size_t column = 0; column < state.fields.size(); ++column) {
    if (state.direct.at(column)) {
      continue;
    }
    ArrowArray* child = root->children[column];
    Check(ArrowArrayStartAppending(child), error);
    state.chunks->Runs(offset, count,
                       [&](duckdb_data_chunk chunk, size_t begin, size_t take, size_t) {
                         for (size_t row = begin; row < begin + take; ++row) {
                           AppendArrowVector(child, state.fields.at(column),
                                             duckdb_data_chunk_get_vector(chunk, column),
                                             state.chunks->type(column), row);
                         }
                       });
    Check(ArrowArrayFinishBuildingDefault(child, &error), error);
  }
  if (!state.direct_types.empty()) {
    // nanoarrow 0.7 cannot encode nonzero array offsets. Copy the selected rows in DuckDB,
    // including ranges crossing chunk boundaries, before exporting offset-zero Arrow arrays.
    const Chunk selected(
        duckdb_create_data_chunk(state.direct_types.data(), state.direct_types.size()));
    const Handle<duckdb_selection_vector, DestroySelection> selection(
        duckdb_create_selection_vector(count));
    auto* indices = duckdb_selection_vector_get_data_ptr(selection.get());
    state.chunks->Runs(
        offset, count, [&](duckdb_data_chunk chunk, size_t begin, size_t take, size_t copied) {
          for (size_t i = 0; i < take; ++i) {
            indices[i] = static_cast<sel_t>(begin + i);
          }
          for (size_t column = 0, target = 0; column < state.fields.size(); ++column) {
            if (state.direct.at(column)) {
              duckdb_vector_copy_sel(duckdb_data_chunk_get_vector(chunk, column),
                                     duckdb_data_chunk_get_vector(selected.get(), target++),
                                     selection.get(), take, 0, copied);
            }
          }
        });
    duckdb_data_chunk_set_size(selected.get(), count);
    nanoarrow::UniqueArray exported;
    Check(duckdb_data_chunk_to_arrow(state.options.get(), selected.get(), exported.get()));
    for (size_t column = 0, source = 0; column < state.fields.size(); ++column) {
      if (!state.direct.at(column)) {
        continue;
      }
      Normalize(exported->children[source], *state.wire->children[column], state.fields.at(column));
      nanoarrow::UniqueArrayView view;
      Check(ArrowArrayViewInitFromSchema(view.get(), state.wire->children[column], &error), error);
      Check(ArrowArrayViewSetArray(view.get(), exported->children[source], &error), error);
      for (int64_t row = 0; std::cmp_less(row, count); ++row) {
        ValidateCivilTime(*view.get(), state.fields.at(column), row);
      }
      // Both producers release children that were not moved out of them.
      ArrowArrayRelease(root->children[column]);
      ArrowArrayMove(exported->children[source++], root->children[column]);
    }
  }
  root->length = static_cast<int64_t>(count);
  root->null_count = 0;
  return arrow ? EncodeArrowRecordBatch(*state.wire.get(), *root.get())
               : EncodeAvroRows(*state.wire.get(), *root.get(), state.fields);
}

}  // namespace bigquery_emulator_duckdb
