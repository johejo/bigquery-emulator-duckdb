#pragma once

#include <vector>

#include "src/field_schema.h"
#include "src/gcs.h"
#include "src/statement_resources.h"
#include "src/table_metadata.h"

// Object tables, which list the objects of Cloud Storage with their metadata:
// third_party/bigquery_docs/object-table-introduction.md

namespace bigquery_emulator_duckdb {

// The fixed schema of an object table, without the data pseudocolumn, which queries cannot read.
std::vector<FieldSchema> ObjectTableSchema();

// A row for each object that a URI of the object table `definition` names, read when a statement
// reads the table. Each object appears once, however many URIs name it.
TableRead ObjectTableRead(GcsExternalData definition, std::vector<FieldSchema> schema,
                          GcsClient client);

}  // namespace bigquery_emulator_duckdb
