#pragma once

#include <map>
#include <string>
#include <vector>

#include "src/information_schema_rows.h"

namespace bigquery_emulator_duckdb::information_schema {

// An option as SCHEMATA_OPTIONS and TABLE_OPTIONS list it: its name, its type and its value as
// a GoogleSQL literal.
struct Option {
  std::string name;
  std::string type;
  std::string value;
};

std::string StringOptionValue(const std::string& value);
// The metadata options shared by datasets, tables and views.
std::vector<Option> CommonOptions(const std::string& description, const std::string& friendly_name,
                                  const std::map<std::string, std::string>& labels,
                                  const std::string& default_rounding_mode,
                                  const std::vector<std::string>& label_order = {});
// Appends options in name order, prefixed with the dataset or table identity.
void AppendOptions(const Row& identity, std::vector<Option> options, std::vector<Row>& rows);

}  // namespace bigquery_emulator_duckdb::information_schema
