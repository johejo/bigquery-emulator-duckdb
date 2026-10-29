// Checks tools/ddl_options.json, the DDL option catalog tools/restprobe probes, against GoogleSQL
// and the options the DDL handlers implement. internal/ddloptions checks its structure.
#include <algorithm>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/analyzer.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/analyzer_output.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/type_factory.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/analyzer.h"
#include "src/catalog.h"
#include "src/ddl_option_types.h"
#include "src/field_schema.h"
#include "src/table_source.h"
#include "src/timestamp_precision.h"

namespace bigquery_emulator_duckdb {
namespace {
using nlohmann::json;

class NoTables : public TableSource {
 public:
  std::optional<std::vector<FieldSchema>> FindTable(const std::string& /*project*/,
                                                    const std::string& /*dataset*/,
                                                    const std::string& /*table*/) override {
    return std::nullopt;
  }
};

json Catalog() {
  std::ifstream input("tools/ddl_options.json");
  return json::parse(input);
}

bool Applies(const json& context, const std::string& name) {
  const auto contains = [&](const char* key) {
    const auto names = context.value(key, json::array());
    return std::ranges::any_of(names, [&](const auto& candidate) { return candidate == name; });
  };
  return (!context.contains("only") || contains("only")) && !contains("exclude");
}

// The first problem with the catalog: a type GoogleSQL cannot parse, a sample that does not
// coerce to its option's type, a type that differs from the handlers', or an option the handlers
// implement that the catalog leaves out. Empty when there is none.
std::string Problem(const json& catalog) {
  googlesql::TypeFactory factory;
  NoTables source;
  BigQueryCatalog tables(source, &factory, "test", "d", TimestampPrecision::kMicroseconds);
  const auto settings = ScriptAnalyzerOptions({
      .default_project = "test",
      .default_dataset = "d",
      .timestamp_precision = TimestampPrecision::kMicroseconds,
  });
  if (!settings.ok()) {
    return settings.status().ToString();
  }
  const auto type_of = [&](const json& option, const googlesql::Type*& type) {
    return googlesql::AnalyzeType(option.at("type"), *settings, &tables, &factory, &type);
  };
  for (const auto& [group, options] : catalog.at("groups").items()) {
    for (const auto& option : options) {
      const std::string name = group + "." + option.at("name").get<std::string>();
      const googlesql::Type* type = nullptr;
      if (const auto status = type_of(option, type); !status.ok()) {
        return name + ": " + status.ToString();
      }
      for (const auto& value : option.value("values", json::array())) {
        std::unique_ptr<const googlesql::AnalyzerOutput> analyzed;
        const auto status = googlesql::AnalyzeExpressionForAssignmentToType(
            value.get<std::string>(), *settings, &tables, &factory, type, std::nullopt, &analyzed);
        if (!status.ok()) {
          return name + ": " + status.ToString();
        }
      }
    }
  }
  std::set<std::pair<std::string, std::string>> covered;
  for (const auto& context : catalog.at("contexts")) {
    const std::string implementation = context.value("implementation", "");
    for (const auto& option : catalog.at("groups").at(context.at("group").get<std::string>())) {
      const std::string name = option.at("name");
      const auto parsed = ParseDdlOptionContext(implementation);
      const auto* definition = parsed ? FindDdlOption(*parsed, name) : nullptr;
      if (definition == nullptr || !Applies(context, name)) {
        continue;
      }
      const googlesql::Type* type = nullptr;
      const auto* actual = DdlOptionSqlType(definition->type);
      if (!type_of(option, type).ok() || actual == nullptr || !actual->Equals(type)) {
        return context.at("name").get<std::string>() + "." + name +
               ": catalog type differs from runtime type";
      }
      covered.emplace(implementation, name);
    }
  }
  const auto definitions = DdlOptionDefinitions();
  const auto missing = std::ranges::find_if(definitions, [&](const auto& definition) {
    return !covered.contains({std::string(DdlOptionContextName(definition.context)),
                              std::string(DdlOptionName(definition.name))});
  });
  if (missing != definitions.end()) {
    return "runtime option missing from catalog: " +
           std::string(DdlOptionContextName(missing->context)) + "." +
           std::string(DdlOptionName(missing->name));
  }
  return "";
}

TEST(DdlOptionsCatalog, AgreesWithGoogleSqlAndRuntime) { EXPECT_EQ(Problem(Catalog()), ""); }

TEST(DdlOptionsCatalog, RejectsTypeDriftEvenWithoutSamples) {
  auto catalog = Catalog();
  for (auto& option : catalog["groups"]["schema_option_list"]) {
    if (option["name"] == "description") {
      option["type"] = "INT64";
      option.erase("values");
    }
  }
  EXPECT_NE(Problem(catalog).find("catalog type differs from runtime type"), std::string::npos);
}

TEST(DdlOptionsCatalog, RejectsMissingRuntimeOption) {
  auto catalog = Catalog();
  auto& options = catalog["groups"]["schema_option_list"];
  options.erase(
      std::ranges::find_if(options, [](const auto& o) { return o["name"] == "location"; }));
  EXPECT_EQ(Problem(catalog), "runtime option missing from catalog: CREATE SCHEMA.location");
}

TEST(DdlOptionsCatalog, RejectsInvalidSamplesEvenWithoutStatementTemplate) {
  auto catalog = Catalog();
  catalog["groups"]["external_schema_option_list"][0]["values"] = {"SELECT"};
  EXPECT_NE(Problem(catalog).find("external_schema_option_list.description"), std::string::npos);
}

TEST(DdlOptionsCatalog, RejectsUnparsableTypes) {
  auto catalog = Catalog();
  catalog["groups"]["schema_option_list"][0]["type"] = "NOT_A_TYPE";
  EXPECT_NE(Problem(catalog).find("schema_option_list.default_kms_key_name"), std::string::npos);
}
}  // namespace
}  // namespace bigquery_emulator_duckdb
