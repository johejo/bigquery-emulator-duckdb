#include "src/result_chunks.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/query_result.hpp"
#include "src/backend_error.h"

namespace bigquery_emulator_duckdb {

class ResultChunks::Impl {
 public:
  duckdb::vector<duckdb::LogicalType> types;
  std::vector<duckdb::unique_ptr<duckdb::DataChunk>> chunks;
};

ResultChunks::ResultChunks(duckdb::QueryResult& result, std::shared_ptr<const void> owner)
    : owner_(std::move(owner)), impl_(std::make_unique<Impl>()) {
  impl_->types = result.GetTypes();
  size_t count = 0;
  while (auto chunk = result.Fetch()) {
    // Normalize recursively before readers share the buffers. Reading a shared result must
    // never lazily flatten a constant, dictionary or nested child vector.
    chunk->Flatten();
    count += chunk->size();
    impl_->chunks.push_back(std::move(chunk));
    ends_.push_back(count);
  }
  if (result.HasError()) {
    throw BackendError(result.GetError());
  }
}

ResultChunks::~ResultChunks() = default;

size_t ResultChunks::column_count() const { return impl_->types.size(); }

const duckdb::LogicalType& ResultChunks::type(size_t column) const {
  return impl_->types.at(column);
}

const duckdb::DataChunk& ResultChunks::ChunkAt(size_t index) const {
  return *impl_->chunks.at(index);
}

}  // namespace bigquery_emulator_duckdb
