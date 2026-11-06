#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "src/field_schema.h"
#include "src/file_read.h"
#include "src/file_read_options.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {

// BigQuery's message of a bad CSV record that starts on `line` at byte `offset`, for the reason
// `detail`, naming the column `index`, of `field`, when it is given, and the record's `value` of
// it, if it has one.
std::string CsvRecordError(std::string_view detail, const RecordPosition& position,
                           const FieldSchema* field = nullptr, size_t index = 0,
                           const std::optional<std::string>& value = std::nullopt);

// Reads CSV `sources` with `options` as BigQuery does for `schema`, and writes copies that
// FileReadQuery reads: comma-delimited records of every column, each value quoted and each NULL
// an unquoted empty value. Throws ApiError::Invalid for a record BigQuery rejects, unless `bad`
// adds it and may skip it, which leaves it out. With `bad`, each kept record's number comes first
// when options.numbered_records is set, and its position is recorded. With `columns`, external
// values are converted only for those columns, as for the typed reader's column projection.
std::vector<FileSource> StageCsvSources(
    const std::vector<FileSource>& sources, const CsvReadOptions& options,
    const std::vector<FieldSchema>& schema, TemporaryFiles& files, BadRecords* bad = nullptr,
    const std::optional<std::set<std::string>>& columns = std::nullopt);

}  // namespace bigquery_emulator_duckdb
