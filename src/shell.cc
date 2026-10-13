#include "src/shell.h"

// include-cleaner requires signal.h for sigaction; modernize-deprecated-headers suggests csignal
// instead, so suppress it on this include.
#include <signal.h>  // NOLINT(modernize-deprecated-headers)
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "googlesql/parser/parser.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/parse_resume_location.h"
#include "googlesql/public/parse_tokens.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/catalog.h"
#include "src/emulator.h"
#include "src/references.h"
#include "src/server/requests.h"
#include "src/server/resources.h"

namespace bigquery_emulator_duckdb {

namespace {

using nlohmann::json;
// Rows keep the schema's field order.
using nlohmann::ordered_json;

// Set by SIGINT, which discards the entry being typed. A signal handler can only set a global.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile sig_atomic_t interrupted = 0;

void Interrupt(int /*signal*/) { interrupted = 1; }

// Without SA_RESTART, so that SIGINT also interrupts a pending read().
void InstallInterruptHandler() {
  struct sigaction action{};
  action.sa_handler = Interrupt;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
}

enum class LineStatus : std::uint8_t { kLine, kEnd, kInterrupted };

// Reads a line from standard input without its newline. Reads byte by byte, since a terminal
// delivers input a line at a time anyway, and an interrupted read() returns at once.
LineStatus ReadLine(std::string& line) {
  line.clear();
  char byte = 0;
  while (true) {
    const auto count = read(STDIN_FILENO, &byte, 1);
    if (count < 0 && errno == EINTR) {
      return LineStatus::kInterrupted;
    }
    if (count <= 0) {
      return line.empty() ? LineStatus::kEnd : LineStatus::kLine;
    }
    if (byte == '\n') {
      return LineStatus::kLine;
    }
    line += byte;
  }
}

std::string_view Trim(std::string_view text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) {
    return {};
  }
  return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

// The value of `cell`, a row's {"v": ...}, with RECORD values as objects keyed by field name and
// REPEATED values as arrays. Scalars stay strings, as the API encodes them.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a field and its value.
ordered_json CellValue(const json& field, const json& value, bool element = false) {
  if (value.is_null()) {
    return nullptr;
  }
  if (!element && field.value("mode", "") == "REPEATED") {
    ordered_json values = ordered_json::array();
    for (const json& item : value) {
      values.push_back(CellValue(field, item.at("v"), true));
    }
    return values;
  }
  const std::string type = field.value("type", "");
  if (type == "RECORD" || type == "STRUCT") {
    ordered_json record = ordered_json::object();
    const json& fields = field.at("fields");
    const json& cells = value.at("f");
    for (size_t i = 0; i < fields.size(); ++i) {
      record[fields[i].at("name").get<std::string>()] = CellValue(fields[i], cells[i].at("v"));
    }
    return record;
  }
  return value;
}

std::vector<ordered_json> RowObjects(const json& response) {
  std::vector<ordered_json> rows;
  const json& fields = response.at("schema").at("fields");
  for (const json& row : response.value("rows", json::array())) {
    ordered_json object = ordered_json::object();
    const json& cells = row.at("f");
    for (size_t i = 0; i < fields.size(); ++i) {
      object[fields[i].at("name").get<std::string>()] = CellValue(fields[i], cells[i].at("v"));
    }
    rows.push_back(std::move(object));
  }
  return rows;
}

// A scalar as its text and anything else as JSON; `null_text` stands for NULL.
std::string CellText(const ordered_json& value, std::string_view null_text) {
  if (value.is_null()) {
    return std::string(null_text);
  }
  return value.is_string() ? value.get<std::string>() : value.dump();
}

// The width of UTF-8 `text` in code points.
size_t Width(std::string_view text) {
  return static_cast<size_t>(std::ranges::count_if(
      text, [](char byte) { return (static_cast<unsigned char>(byte) & 0xC0U) != 0x80U; }));
}

void WritePretty(const std::vector<std::string>& names, const std::vector<ordered_json>& rows,
                 std::ostream& out) {
  std::vector<std::vector<std::string>> cells;
  std::vector<size_t> widths;
  std::ranges::transform(names, std::back_inserter(widths),
                         [](const std::string& name) { return Width(name); });
  for (const ordered_json& row : rows) {
    std::vector<std::string>& texts = cells.emplace_back();
    for (size_t i = 0; i < names.size(); ++i) {
      texts.push_back(CellText(row.at(names.at(i)), "NULL"));
      widths.at(i) = std::max(widths.at(i), Width(texts.back()));
    }
  }
  std::string rule = "+";
  for (const size_t width : widths) {
    rule += std::string(width + 2, '-') + "+";
  }
  const auto write_row = [&](const std::vector<std::string>& texts) {
    out << '|';
    for (size_t i = 0; i < texts.size(); ++i) {
      out << ' ' << texts.at(i) << std::string(widths.at(i) - Width(texts.at(i)) + 1, ' ') << '|';
    }
    out << '\n';
  };
  out << rule << '\n';
  write_row(names);
  out << rule << '\n';
  for (const auto& texts : cells) {
    write_row(texts);
  }
  if (!cells.empty()) {
    out << rule << '\n';
  }
}

// RFC 4180 quoting; NULL is an empty field.
void WriteCsvField(const ordered_json& value, std::ostream& out) {
  const std::string text = CellText(value, "");
  if (text.find_first_of(",\"\r\n") == std::string::npos) {
    out << text;
    return;
  }
  out << '"';
  for (const char c : text) {
    if (c == '"') {
      out << '"';
    }
    out << c;
  }
  out << '"';
}

void WriteCsv(const std::vector<std::string>& names, const std::vector<ordered_json>& rows,
              std::ostream& out) {
  for (size_t i = 0; i < names.size(); ++i) {
    out << (i == 0 ? "" : ",");
    WriteCsvField(names.at(i), out);
  }
  out << "\r\n";
  for (const ordered_json& row : rows) {
    for (size_t i = 0; i < names.size(); ++i) {
      out << (i == 0 ? "" : ",");
      WriteCsvField(row.at(names.at(i)), out);
    }
    out << "\r\n";
  }
}

// Whether an interactive entry is ready to run: it ends with a semicolon outside comments and
// literals, and a block such as BEGIN ... END is closed. Other syntax errors also end it, so that
// the job reports them.
bool IsCompleteEntry(std::string_view input) {
  googlesql::ParseTokenOptions options;
  options.language_options = GoogleSqlLanguageOptions();
  auto location = googlesql::ParseResumeLocation::FromStringView(input);
  std::vector<googlesql::ParseToken> tokens;
  if (const absl::Status status = googlesql::GetParseTokens(options, &location, &tokens);
      !status.ok()) {
    return !absl::StrContains(status.message(), "Unclosed triple-quoted string literal") &&
           !absl::StrContains(status.message(), "Unclosed comment");
  }
  const auto last = std::ranges::find_if(tokens.rbegin(), tokens.rend(),
                                         [](const auto& token) { return !token.IsEndOfInput(); });
  if (last == tokens.rend() || !last->IsKeyword() || last->GetKeyword() != ";") {
    return false;
  }
  std::unique_ptr<googlesql::ParserOutput> output;
  const absl::Status status =
      googlesql::ParseScript(input, googlesql::ParserOptions(GoogleSqlLanguageOptions()),
                             {.mode = googlesql::ERROR_MESSAGE_ONE_LINE}, &output);
  return status.ok() || !absl::StrContains(status.message(), "Unexpected end of script");
}

// Writes the rows of a jobs.query response, whose values keep the API's encoding.
void WriteRows(const json& response, OutputFormat format, std::ostream& out) {
  std::vector<std::string> names;
  std::ranges::transform(response.at("schema").at("fields"), std::back_inserter(names),
                         [](const json& field) { return field.at("name").get<std::string>(); });
  const std::vector<ordered_json> rows = RowObjects(response);
  switch (format) {
    case OutputFormat::kPretty:
      WritePretty(names, rows, out);
      break;
    case OutputFormat::kJson:
      out << ordered_json(rows).dump() << '\n';
      break;
    case OutputFormat::kJsonl:
      for (const ordered_json& row : rows) {
        out << row.dump() << '\n';
      }
      break;
    case OutputFormat::kCsv:
      WriteCsv(names, rows, out);
      break;
  }
}

class Shell {
 public:
  Shell(Emulator& emulator, std::string project_id, std::string dataset_id, OutputFormat format)
      : emulator_(emulator),
        project_id_(std::move(project_id)),
        dataset_id_(std::move(dataset_id)),
        format_(format) {}

  // Runs `query` as one job and prints its result; returns whether the job succeeded.
  bool Run(const std::string& query) {
    QueryRequest request;
    request.project_id = project_id_;
    request.query = query;
    if (!dataset_id_.empty()) {
      request.default_dataset =
          DatasetReference{.project_id = project_id_, .dataset_id = dataset_id_};
    }
    server::ResultPage page;
    page.max_results = std::numeric_limits<int64_t>::max();
    json response;
    try {
      const auto job = emulator_.RunQuery(std::move(request));
      response = server::QueryResponse(*job, page);
    } catch (const std::exception& error) {
      std::cerr << "Error: " << error.what() << '\n';
      return false;
    }
    if (response.contains("schema")) {
      WriteRows(response, format_, std::cout);
      std::cout.flush();
    }
    if (response.contains("numDmlAffectedRows")) {
      std::cerr << response["numDmlAffectedRows"].get<std::string>() << " rows affected\n";
    }
    return true;
  }

  // Reads entries from a terminal until the end of input or .quit.
  void Interact() {
    InstallInterruptHandler();
    std::string entry;
    std::string line;
    while (true) {
      std::cerr << (entry.empty() ? Prompt() + "> " : std::string(Prompt().size(), ' ') + "-> ");
      const LineStatus status = ReadLine(line);
      if (status == LineStatus::kInterrupted || interrupted != 0) {
        interrupted = 0;
        entry.clear();
        std::cerr << '\n';
        continue;
      }
      if (status == LineStatus::kEnd) {
        std::cerr << '\n';
        return;
      }
      if (entry.empty()) {
        const std::string_view command = Trim(line);
        if (command.empty()) {
          continue;
        }
        if (command.front() == '.' || command.front() == '\\') {
          if (!RunCommand(command.substr(1))) {
            return;
          }
          continue;
        }
      }
      entry += line;
      entry += '\n';
      if (IsCompleteEntry(entry)) {
        Run(entry);
        entry.clear();
        // A SIGINT while the job ran only discards what was typed meanwhile.
        interrupted = 0;
      }
    }
  }

 private:
  [[nodiscard]] std::string Prompt() const {
    return dataset_id_.empty() ? project_id_ : project_id_ + "." + dataset_id_;
  }

  // Runs a command without its prefix; returns false to quit.
  bool RunCommand(std::string_view command) {
    const auto space = command.find_first_of(" \t");
    const std::string_view name = command.substr(0, space);
    const std::string_view argument =
        space == std::string_view::npos ? std::string_view() : Trim(command.substr(space));
    if (name == "quit") {
      return false;
    }
    if (name == "help") {
      std::cerr << "Enter a query or script ending with ';' to run it as one job.\n"
                   "Commands, starting with '.' or '\\':\n"
                   "  .help                     Show this help\n"
                   "  .quit                     Exit\n"
                   "  .use PROJECT[.DATASET]    Set the default project and dataset\n"
                   "  .format pretty|json|jsonl|csv  Set the output format\n";
    } else if (name == "use" && !argument.empty()) {
      Use(std::string(argument));
    } else if (const auto format = ParseOutputFormat(argument);
               name == "format" && format.has_value()) {
      format_ = *format;
    } else {
      std::cerr << "Error: Invalid command: " << command << "; enter .help for commands\n";
    }
    return true;
  }

  // Project IDs can contain dots, so a registered project takes precedence over a dataset.
  void Use(const std::string& target) {
    const auto dot = target.rfind('.');
    try {
      project_id_ = emulator_.ResolveProject(target);
      dataset_id_.clear();
      return;
    } catch (const ApiError& error) {
      if (dot == std::string::npos || dot + 1 == target.size()) {
        std::cerr << "Error: " << error.what() << '\n';
        return;
      }
    }
    try {
      project_id_ = emulator_.ResolveProject(target.substr(0, dot));
      dataset_id_ = target.substr(dot + 1);
    } catch (const ApiError& error) {
      std::cerr << "Error: " << error.what() << '\n';
    }
  }

  Emulator& emulator_;
  std::string project_id_;
  std::string dataset_id_;
  OutputFormat format_;
};

}  // namespace

std::optional<OutputFormat> ParseOutputFormat(std::string_view name) {
  if (name == "pretty") {
    return OutputFormat::kPretty;
  }
  if (name == "json") {
    return OutputFormat::kJson;
  }
  if (name == "jsonl") {
    return OutputFormat::kJsonl;
  }
  if (name == "csv") {
    return OutputFormat::kCsv;
  }
  return std::nullopt;
}

int RunShell(Emulator& emulator, const ShellOptions& options) {
  std::string project_id = options.project_id;
  if (project_id.empty()) {
    const auto& projects = emulator.ListProjects();
    if (projects.size() != 1) {
      std::cerr << "--project-id is required unless exactly one project is registered\n";
      return 2;
    }
    project_id = projects.front().project_id;
  } else {
    try {
      project_id = emulator.ResolveProject(project_id);
    } catch (const ApiError& error) {
      std::cerr << error.what() << '\n';
      return 2;
    }
  }
  const OutputFormat format = options.format.value_or(
      isatty(STDOUT_FILENO) != 0 ? OutputFormat::kPretty : OutputFormat::kJsonl);
  Shell shell(emulator, project_id, options.dataset_id, format);
  if (options.execute.has_value()) {
    return shell.Run(*options.execute) ? 0 : 1;
  }
  if (isatty(STDIN_FILENO) != 0) {
    shell.Interact();
    return 0;
  }
  const std::string input(std::istreambuf_iterator<char>(std::cin), {});
  if (Trim(input).empty()) {
    return 0;
  }
  return shell.Run(input) ? 0 : 1;
}

}  // namespace bigquery_emulator_duckdb
