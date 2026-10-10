#pragma once

#include <map>
#include <string>
#include <vector>

namespace googlesql {
class Value;
struct StringVectorCaseLess;
// As googlesql/public/analyzer.h declares it, which takes seconds to parse.
using SystemVariableValuesMap = std::map<std::vector<std::string>, Value, StringVectorCaseLess>;
}  // namespace googlesql
