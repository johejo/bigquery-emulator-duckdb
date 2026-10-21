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
#include "src/duckdb_handle.h"
#include "src/field_schema.h"
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
    case FieldType::kRecord:
      return "+s";
    default:
      return {};
  }
}

bool Eligible(const FieldSchema& field) {
  // DuckDB can preserve NULL lists where BigQuery returns empty ones. Those, BIGNUMERIC and
  // other logical types go through their cells.
  return field.mode != FieldMode::kRepeated && !Format(field.type).empty() &&
         std::ranges::all_of(field.fields, Eligible);
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
      return true;
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

// The cell codec parses civil times with GoogleSQL. Keep those range checks when their physical
// Arrow buffers are passed through, including for externally created tables.
void ValidateCivilTime(const ArrowArrayView& view, const FieldSchema& field, int64_t row) {
  if (ArrowArrayViewIsNull(&view, row) != 0) {
    return;
  }
  if (field.type == FieldType::kRecord) {
    for (size_t i = 0; i < field.fields.size(); ++i) {
      ValidateCivilTime(*view.children[i], field.fields.at(i), row);
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
  std::vector<LogicalType> types;
  // Whether DuckDB exports each column in the layout Storage Read gives it.
  std::vector<bool> direct;
  std::vector<duckdb_logical_type> direct_types;
  nanoarrow::UniqueSchema wire;
  std::vector<Chunk> chunks;
  std::vector<size_t> ends;

  // Calls visit(chunk, begin, take, copied) for each run of rows [offset, offset + count) in
  // one chunk, where `copied` rows precede it.
  template <typename F>
  void Runs(size_t offset, size_t count, const F& visit) const {
    size_t copied = 0;
    size_t index = std::ranges::upper_bound(ends, offset) - ends.begin();
    while (copied < count) {
      const size_t begin = index == 0 ? 0 : ends.at(index - 1);
      const size_t take = std::min(count - copied, ends.at(index) - offset);
      visit(chunks.at(index).get(), offset - begin, take, copied);
      copied += take;
      offset += take;
      ++index;
    }
  }
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
  StorageArrowSchema(state.wire.get(), state.fields);
  for (idx_t i = 0; i < state.fields.size(); ++i) {
    state.types.emplace_back(duckdb_column_logical_type(&result, i));
    duckdb_logical_type type = state.types.back().get();
    bool direct = Eligible(state.fields.at(i)) && NativeStorage(type);
    if (direct) {
      // Some DuckDB types have no Arrow representation; their export fails and they use cells.
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
  size_t count = 0;
  while (true) {
    Chunk chunk(duckdb_fetch_chunk(result));
    if (chunk.get() == nullptr) {
      break;
    }
    count += duckdb_data_chunk_get_size(chunk.get());
    state.chunks.push_back(std::move(chunk));
    state.ends.push_back(count);
  }
}

StorageSnapshot::~StorageSnapshot() = default;

const std::vector<FieldSchema>& StorageSnapshot::schema() const { return impl_->fields; }

size_t StorageSnapshot::row_count() const { return impl_->ends.empty() ? 0 : impl_->ends.back(); }

std::string StorageSnapshot::ReadRows(size_t offset, size_t count, bool arrow) const {
  auto& state = *impl_;
  if (offset > row_count() || count > row_count() - offset || count > duckdb_vector_size()) {
    throw std::out_of_range("Storage Read row range is outside the snapshot or vector capacity");
  }
  const auto cell = [&](duckdb_data_chunk chunk, size_t column, size_t row) {
    return ToCell(duckdb_data_chunk_get_vector(chunk, column), state.types.at(column).get(), row,
                  false)
        .at("v");
  };
  if (!arrow) {
    std::string output;
    state.Runs(offset, count, [&](duckdb_data_chunk chunk, size_t begin, size_t take, size_t) {
      for (size_t row = begin; row < begin + take; ++row) {
        for (size_t column = 0; column < state.fields.size(); ++column) {
          AppendAvroCell(output, state.fields.at(column), cell(chunk, column, row));
        }
      }
    });
    return output;
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
    state.Runs(offset, count, [&](duckdb_data_chunk chunk, size_t begin, size_t take, size_t) {
      for (size_t row = begin; row < begin + take; ++row) {
        AppendArrowCell(child, state.fields.at(column), cell(chunk, column, row));
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
    state.Runs(offset, count,
               [&](duckdb_data_chunk chunk, size_t begin, size_t take, size_t copied) {
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
  return EncodeArrowRecordBatch(*state.wire.get(), *root.get());
}

}  // namespace bigquery_emulator_duckdb
