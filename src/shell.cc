#include "src/shell.h"

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
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
#include "src/emulator.h"
#include "src/language_options.h"
#include "src/references.h"
#include "src/server/requests.h"
#include "src/server/resources.h"
#include "src/shell/format.h"
#include "src/shell/repl.h"

namespace bigquery_emulator_duckdb {

namespace {

using nlohmann::json;

std::string_view Trim(std::string_view text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) {
    return {};
  }
  return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
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

class Shell {
 public:
  Shell(Emulator& emulator, std::string project_id, std::string dataset_id,
        shell::OutputFormat format)
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
      shell::WriteRows(response, format_, std::cout);
      std::cout.flush();
    }
    if (response.contains("numDmlAffectedRows")) {
      std::cerr << response["numDmlAffectedRows"].get<std::string>() << " rows affected\n";
    }
    return true;
  }

  // Reads entries from a terminal until the end of input or .quit.
  void Interact() {
    shell::Interact({
        .prompt = [this] { return Prompt(); },
        .is_complete = IsCompleteEntry,
        .run = [this](const std::string& entry) { Run(entry); },
        .command = [this](std::string_view command) { return RunCommand(command); },
    });
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
    } else if (const auto format = shell::ParseOutputFormat(argument);
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
  shell::OutputFormat format_;
};

}  // namespace

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
  const shell::OutputFormat format = options.format.value_or(
      isatty(STDOUT_FILENO) != 0 ? shell::OutputFormat::kPretty : shell::OutputFormat::kJsonl);
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
