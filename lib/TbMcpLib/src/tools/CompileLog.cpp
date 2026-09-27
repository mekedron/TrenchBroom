/*
 Copyright (C) 2026 Nikita Rabykin

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "mcp/tools/CompileLog.h"

#include "mcp/tools/CompileUtils.h"

#include "kd/string_compare.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"

#include <fmt/format.h>

#include <algorithm>

namespace tb::mcp
{
namespace
{

struct Line
{
  std::string_view text;
  /** Offset of the line in the log. */
  size_t offset = 0;
};

/** The lines of the log without line terminators; a trailing newline adds no line. */
std::vector<Line> splitLines(const std::string_view log)
{
  auto result = std::vector<Line>{};
  auto start = size_t{0};
  while (start < log.size())
  {
    const auto end = log.find('\n', start);
    auto text = log.substr(start, end == std::string_view::npos ? end : end - start);
    if (!text.empty() && text.back() == '\r')
    {
      text.remove_suffix(1);
    }
    result.push_back(Line{text, start});
    if (end == std::string_view::npos)
    {
      break;
    }
    start = end + 1;
  }
  return result;
}

std::string_view trim(std::string_view str)
{
  constexpr auto whitespace = std::string_view{" \t\r\n"};
  const auto first = str.find_first_not_of(whitespace);
  if (first == std::string_view::npos)
  {
    return {};
  }
  const auto last = str.find_last_not_of(whitespace);
  return str.substr(first, last - first + 1);
}

/** Removes leading and trailing '*' and whitespace, e.g. "*** ERROR ***" -> "ERROR". */
std::string_view trimStars(std::string_view str)
{
  constexpr auto chars = std::string_view{"* \t"};
  const auto first = str.find_first_not_of(chars);
  if (first == std::string_view::npos)
  {
    return {};
  }
  const auto last = str.find_last_not_of(chars);
  return str.substr(first, last - first + 1);
}

bool startsWith(std::string_view str, std::string_view prefix)
{
  return kdl::cs::str_is_prefix(str, prefix);
}

bool startsWithCi(std::string_view str, std::string_view prefix)
{
  return kdl::ci::str_is_prefix(str, prefix);
}

bool containsCi(std::string_view str, std::string_view needle)
{
  return kdl::ci::str_contains(str, needle);
}

/** Removes the quotes that fmt may put around a formatted path. */
std::string unquotePath(std::string_view str)
{
  str = trim(str);
  if (str.size() >= 2 && str.front() == '"' && str.back() == '"')
  {
    auto result = std::string{};
    for (size_t i = 1; i + 1 < str.size(); ++i)
    {
      if (str[i] == '\\' && i + 2 < str.size())
      {
        ++i;
      }
      result.push_back(str[i]);
    }
    return result;
  }
  return std::string{str};
}

/** The paths of a list written as "a, b, c"; empty if the list is empty. */
std::vector<std::filesystem::path> parsePathList(std::string_view str)
{
  auto result = std::vector<std::filesystem::path>{};
  if (trim(str).empty())
  {
    return result;
  }
  constexpr auto separator = std::string_view{", "};
  auto start = size_t{0};
  while (start <= str.size())
  {
    const auto end = str.find(separator, start);
    const auto item =
      str.substr(start, end == std::string_view::npos ? end : end - start);
    if (const auto path = unquotePath(item); !path.empty())
    {
      result.emplace_back(path);
    }
    if (end == std::string_view::npos)
    {
      break;
    }
    start = end + separator.size();
  }
  return result;
}

/** The text between the first and the last single quote after the prefix. */
std::string quotedValue(std::string_view line, std::string_view prefix)
{
  const auto rest = line.substr(prefix.size());
  const auto last = rest.rfind('\'');
  return std::string{last == std::string_view::npos ? rest : rest.substr(0, last)};
}

std::optional<vm::vec3d> parsePosition(std::string_view str)
{
  const auto open = str.find('(');
  const auto close = str.find(')', open == std::string_view::npos ? 0 : open);
  if (open == std::string_view::npos || close == std::string_view::npos)
  {
    return std::nullopt;
  }
  auto inner = std::string{str.substr(open + 1, close - open - 1)};
  std::ranges::replace(inner, ',', ' ');
  const auto parts = kdl::str_split(inner, " \t");
  if (parts.size() != 3)
  {
    return std::nullopt;
  }
  const auto x = kdl::str_to_double(parts[0]);
  const auto y = kdl::str_to_double(parts[1]);
  const auto z = kdl::str_to_double(parts[2]);
  if (!x || !y || !z)
  {
    return std::nullopt;
  }
  return vm::vec3d{*x, *y, *z};
}

enum class RunnerLine
{
  None,
  Start,
  Failure,
  Outcome,
};

struct RunnerLineInfo
{
  RunnerLine kind = RunnerLine::None;
  /** The task type the line belongs to, for start and failure lines. */
  std::string_view type;
};

constexpr auto ExportingPrefix = std::string_view{"#### Exporting map file '"};
constexpr auto CopyingPrefix = std::string_view{"#### Copying to '"};
constexpr auto RenamingPrefix = std::string_view{"#### Renaming '"};
constexpr auto DeletingPrefix = std::string_view{"#### Deleting: "};
constexpr auto ExecutingPrefix = std::string_view{"#### Executing '"};
constexpr auto LaunchingPrefix = std::string_view{"#### Launching engine profile '"};
constexpr auto FinishedPrefix = std::string_view{"#### Finished with exit code "};
constexpr auto CrashedPrefix = std::string_view{"#### Crashed with exit code "};
constexpr auto ProcessErrorPrefix = std::string_view{"#### Error '"};
constexpr auto LaunchFailedPrefix = std::string_view{"#### Launch failed"};
constexpr auto ContinuingLine =
  std::string_view{"#### Continuing despite launch failure"};
constexpr auto LaunchedLine = std::string_view{"#### Launched"};
constexpr auto TerminatedPrefix = std::string_view{"#### Terminated"};
constexpr auto UsingWorkDirPrefix = std::string_view{"#### Using working directory"};
constexpr auto WorkDirMissingPrefix = std::string_view{"#### Error: working directory"};
constexpr auto WorkDirFatalPrefix =
  std::string_view{"#### Error: Could not get determine working directory"};

RunnerLineInfo classifyRunnerLine(std::string_view line)
{
  struct Entry
  {
    std::string_view prefix;
    RunnerLine kind;
    std::string_view type;
  };
  static const auto entries = std::vector<Entry>{
    {ExportingPrefix, RunnerLine::Start, "exportMap"},
    {CopyingPrefix, RunnerLine::Start, "copyFiles"},
    {RenamingPrefix, RunnerLine::Start, "renameFile"},
    {DeletingPrefix, RunnerLine::Start, "deleteFiles"},
    {ExecutingPrefix, RunnerLine::Start, "runTool"},
    {LaunchingPrefix, RunnerLine::Start, "launchEngine"},
    {"#### Export failed", RunnerLine::Failure, "exportMap"},
    {"#### Copy failed", RunnerLine::Failure, "copyFiles"},
    {"#### Rename failed", RunnerLine::Failure, "renameFile"},
    {"#### Delete failed", RunnerLine::Failure, "deleteFiles"},
    {"#### Execution failed", RunnerLine::Failure, "runTool"},
    {LaunchFailedPrefix, RunnerLine::Failure, "launchEngine"},
  };
  for (const auto& entry : entries)
  {
    if (startsWith(line, entry.prefix))
    {
      return {entry.kind, entry.type};
    }
  }
  if (
    startsWith(line, FinishedPrefix) || startsWith(line, CrashedPrefix)
    || startsWith(line, ProcessErrorPrefix) || line == ContinuingLine
    || line == LaunchedLine)
  {
    return {RunnerLine::Outcome, {}};
  }
  return {};
}

/** The message of a runner line, without the "#### " prefix. */
std::string runnerMessage(std::string_view line)
{
  return std::string{trim(line.substr(std::min(line.size(), size_t{5})))};
}

enum class Severity
{
  None,
  Error,
  Warning,
};

struct ToolMessage
{
  Severity severity = Severity::None;
  std::string message;
  /** Whether the message is on the next non-empty line. */
  bool onNextLine = false;
};

/**
 * If the text starts with the keyword (case-insensitive), optionally followed by a number
 * and a colon ("WARNING 13:", "Error:"), returns the rest of the text.
 */
std::optional<std::string_view> afterKeyword(
  std::string_view text, std::string_view keyword)
{
  if (!startsWithCi(text, keyword))
  {
    return std::nullopt;
  }
  auto rest = text.substr(keyword.size());
  auto i = size_t{0};
  while (i < rest.size() && rest[i] == ' ')
  {
    ++i;
  }
  while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9')
  {
    ++i;
  }
  while (i < rest.size() && rest[i] == ' ')
  {
    ++i;
  }
  if (i < rest.size() && rest[i] == ':')
  {
    return trim(rest.substr(i + 1));
  }
  return std::nullopt;
}

ToolMessage classifyToolLine(std::string_view line)
{
  const auto text = trimStars(line);
  if (text.empty())
  {
    return {};
  }

  // banners such as "************ ERROR ************"
  if (kdl::ci::str_is_equal(text, "error") && line.find('*') != std::string_view::npos)
  {
    return {Severity::Error, {}, true};
  }

  for (const auto& keyword : {"fatal error", "fatal", "error"})
  {
    if (const auto rest = afterKeyword(text, keyword))
    {
      return {Severity::Error, std::string{*rest}, rest->empty()};
    }
  }
  if (const auto rest = afterKeyword(text, "warning"))
  {
    return {Severity::Warning, std::string{*rest}, rest->empty()};
  }
  return {};
}

bool isLeakBanner(std::string_view line)
{
  const auto text = trimStars(line);
  return line.find('*') != std::string_view::npos
         && (kdl::ci::str_is_equal(text, "leaked") || kdl::ci::str_is_equal(text, "leak"));
}

bool isLeakLine(std::string_view line, const ToolMessage& message)
{
  return containsCi(line, "=== leak") || containsCi(line, "reached occupant")
         || containsCi(line, "leak file written") || containsCi(line, "leakfile written")
         || isLeakBanner(line)
         || (message.severity != Severity::None && containsCi(message.message, "leak"));
}

/** The point file named by the line, if any. */
std::optional<std::filesystem::path> leakPointFile(std::string_view line)
{
  for (const auto& marker : {"leak file written to", "leakfile written to"})
  {
    auto lower = kdl::str_to_lower(line);
    if (const auto pos = lower.find(marker); pos != std::string::npos)
    {
      auto path = trim(line.substr(pos + std::string_view{marker}.size()));
      while (!path.empty() && (path.back() == '.' || path.back() == ':'))
      {
        path.remove_suffix(1);
      }
      if (const auto unquoted = unquotePath(path); !unquoted.empty())
      {
        return std::filesystem::path{unquoted};
      }
    }
  }

  // "Writing x.pts" / "writing x.lin"
  const auto text = trim(line);
  if (startsWithCi(text, "writing "))
  {
    const auto path = trim(text.substr(8));
    if (
      kdl::ci::str_is_suffix(path, ".pts") || kdl::ci::str_is_suffix(path, ".lin")
      || kdl::ci::str_is_suffix(path, ".pts\"") || kdl::ci::str_is_suffix(path, ".lin\""))
    {
      return std::filesystem::path{unquotePath(path)};
    }
  }
  return std::nullopt;
}

void addLeakDetails(CompileLeak& leak, std::string_view line)
{
  const auto text = trim(line);

  // VHLT / ZHLT: "Entity info_player_start @ (  -64, -64,  36)"
  if (!leak.entity && startsWithCi(text, "entity "))
  {
    if (const auto at = text.find(" @ "); at != std::string_view::npos)
    {
      leak.entity = std::string{trim(text.substr(7, at - 7))};
      leak.position = parsePosition(text.substr(at));
    }
  }

  // ericw-tools / tyrutils: Reached occupant "info_player_start" at (-64 -64 36)
  if (const auto lower = kdl::str_to_lower(text);
      lower.find("reached occupant") != std::string::npos)
  {
    const auto pos = lower.find("reached occupant");
    const auto rest = text.substr(pos + 16);
    if (!leak.entity)
    {
      if (const auto open = rest.find('"'); open != std::string_view::npos)
      {
        if (const auto close = rest.find('"', open + 1); close != std::string_view::npos)
        {
          leak.entity = std::string{rest.substr(open + 1, close - open - 1)};
        }
      }
    }
    if (!leak.position)
    {
      if (const auto at = kdl::str_to_lower(rest).find(" at ("); at != std::string::npos)
      {
        leak.position = parsePosition(rest.substr(at));
      }
    }
  }

  if (!leak.pointFile)
  {
    leak.pointFile = leakPointFile(line);
  }
}

struct TaskParseState
{
  bool started = false;
  bool hasOutcome = false;
};

} // namespace

std::string_view toString(const CompileTaskState state)
{
  switch (state)
  {
  case CompileTaskState::Pending:
    return "pending";
  case CompileTaskState::Running:
    return "running";
  case CompileTaskState::Succeeded:
    return "succeeded";
  case CompileTaskState::Failed:
    return "failed";
  case CompileTaskState::Skipped:
    return "skipped";
  }
  return "";
}

CompileLogAnalysis analyzeCompileLog(
  const std::string_view log,
  const std::vector<mdl::CompilationTask>& enabledTasks,
  const bool ended)
{
  auto result = CompileLogAnalysis{};
  for (size_t i = 0; i < enabledTasks.size(); ++i)
  {
    auto& report = result.tasks.emplace_back();
    report.index = i;
    report.type = taskType(enabledTasks[i]);
  }
  auto states = std::vector<TaskParseState>(enabledTasks.size());

  // the task the current lines belong to
  auto current = std::optional<size_t>{};
  // the number of task start or failure-without-start lines seen
  auto assigned = size_t{0};
  auto fatal = false;
  // the index into result.errors of a launch failure that may be followed by
  // "Continuing despite launch failure"
  auto pendingLaunchFailure = std::optional<size_t>{};

  const auto succeed = [&](const size_t index) {
    states[index].hasOutcome = true;
    result.tasks[index].state = CompileTaskState::Succeeded;
  };
  const auto failTask = [&](const size_t index) {
    states[index].hasOutcome = true;
    result.tasks[index].state = CompileTaskState::Failed;
  };

  /** Moves on to the next task; the current task, if open, succeeded. */
  const auto beginTask = [&](const size_t lineNumber) -> std::optional<size_t> {
    if (current && !states[*current].hasOutcome)
    {
      succeed(*current);
    }
    const auto index = assigned++;
    if (index >= enabledTasks.size())
    {
      current = std::nullopt;
      return std::nullopt;
    }
    current = index;
    states[index].started = true;
    result.tasks[index].line = lineNumber;
    result.tasks[index].state = CompileTaskState::Running;
    return index;
  };

  const auto addError = [&](std::string message, const size_t lineNumber) {
    result.errors.push_back(CompileMessage{std::move(message), lineNumber, current});
  };
  const auto addWarning = [&](std::string message, const size_t lineNumber) {
    result.warnings.push_back(CompileMessage{std::move(message), lineNumber, current});
  };

  const auto lines = splitLines(log);
  auto consumed = std::vector<bool>(lines.size(), false);
  auto leakOpen = false;

  for (size_t i = 0; i < lines.size(); ++i)
  {
    const auto line = lines[i].text;
    const auto lineNumber = i + 1;

    if (startsWith(line, "####"))
    {
      leakOpen = false;
      const auto info = classifyRunnerLine(line);
      if (pendingLaunchFailure && line != ContinuingLine)
      {
        pendingLaunchFailure = std::nullopt;
      }

      if (info.kind == RunnerLine::Start)
      {
        const auto index = beginTask(lineNumber);
        if (!index)
        {
          continue;
        }
        auto& report = result.tasks[*index];
        if (startsWith(line, ExportingPrefix))
        {
          const auto path = quotedValue(line, ExportingPrefix);
          report.description = path;
          result.exportedMaps.emplace_back(path);
        }
        else if (startsWith(line, CopyingPrefix))
        {
          const auto rest = line.substr(CopyingPrefix.size());
          const auto separator = rest.find("':");
          const auto dirEnd = rest.find("/':");
          const auto dir = std::string{
            dirEnd != std::string_view::npos      ? rest.substr(0, dirEnd)
            : separator != std::string_view::npos ? rest.substr(0, separator)
                                                  : rest};
          const auto files = separator != std::string_view::npos
                               ? parsePathList(rest.substr(separator + 2))
                               : std::vector<std::filesystem::path>{};
          auto names = std::vector<std::string>{};
          for (const auto& file : files)
          {
            result.copiedFiles.push_back(CompileFileCopy{file, dir});
            names.push_back(file.string());
          }
          report.description =
            files.empty() ? fmt::format("copy nothing (no matching files) to {}", dir)
                          : fmt::format("copy {} to {}", kdl::str_join(names, ", "), dir);
        }
        else if (startsWith(line, RenamingPrefix))
        {
          const auto rest = line.substr(RenamingPrefix.size());
          const auto separator = rest.find("' to '");
          if (separator != std::string_view::npos)
          {
            auto target = rest.substr(separator + 6);
            if (!target.empty() && target.back() == '\'')
            {
              target.remove_suffix(1);
            }
            const auto source = std::string{rest.substr(0, separator)};
            result.renamedFiles.push_back(CompileFileCopy{source, std::string{target}});
            report.description = fmt::format("rename {} to {}", source, target);
          }
          else
          {
            report.description = runnerMessage(line);
          }
        }
        else if (startsWith(line, DeletingPrefix))
        {
          const auto files = parsePathList(line.substr(DeletingPrefix.size()));
          auto names = std::vector<std::string>{};
          for (const auto& file : files)
          {
            names.push_back(file.string());
          }
          report.description = files.empty()
                                 ? std::string{"delete nothing (no matching files)"}
                                 : "delete " + kdl::str_join(names, ", ");
        }
        else if (startsWith(line, ExecutingPrefix))
        {
          report.description = std::string{trim(quotedValue(line, ExecutingPrefix))};
        }
        else if (startsWith(line, LaunchingPrefix))
        {
          const auto rest = line.substr(LaunchingPrefix.size());
          const auto separator = rest.find("' at '");
          if (separator != std::string_view::npos)
          {
            auto path = rest.substr(separator + 6);
            if (!path.empty() && path.back() == '\'')
            {
              path.remove_suffix(1);
            }
            report.description =
              fmt::format("launch {} at {}", rest.substr(0, separator), path);
          }
          else
          {
            report.description = runnerMessage(line);
          }
        }
      }
      else if (info.kind == RunnerLine::Failure)
      {
        // a failure line belongs to the open or failed task of the same type, otherwise
        // it is the failure of the next task that failed before writing its start line
        auto index = std::optional<size_t>{};
        if (
          current && result.tasks[*current].type == info.type
          && (!states[*current].hasOutcome || result.tasks[*current].state == CompileTaskState::Failed))
        {
          index = current;
        }
        else
        {
          index = beginTask(lineNumber);
          if (index)
          {
            result.tasks[*index].description = runnerMessage(line);
          }
        }
        if (index)
        {
          failTask(*index);
        }
        if (info.type == "launchEngine")
        {
          pendingLaunchFailure = result.errors.size();
        }
        addError(runnerMessage(line), lineNumber);
      }
      else if (info.kind == RunnerLine::Outcome)
      {
        if (line == ContinuingLine)
        {
          if (pendingLaunchFailure && *pendingLaunchFailure < result.errors.size())
          {
            auto message = std::move(result.errors[*pendingLaunchFailure]);
            result.errors.erase(
              result.errors.begin() + std::ptrdiff_t(*pendingLaunchFailure));
            result.warnings.push_back(std::move(message));
            pendingLaunchFailure = std::nullopt;
          }
          if (current)
          {
            succeed(*current);
          }
        }
        else if (line == LaunchedLine)
        {
          if (current && !states[*current].hasOutcome)
          {
            succeed(*current);
          }
        }
        else if (startsWith(line, FinishedPrefix) || startsWith(line, CrashedPrefix))
        {
          const auto crashed = startsWith(line, CrashedPrefix);
          const auto exitCode = kdl::str_to_int(
            trim(line.substr(crashed ? CrashedPrefix.size() : FinishedPrefix.size())));
          if (current)
          {
            auto& report = result.tasks[*current];
            report.exitCode = exitCode;
            const auto* runTool =
              std::get_if<mdl::CompilationRunTool>(&enabledTasks[*current]);
            const auto treatAsError = !runTool || runTool->treatNonZeroResultCodeAsError;
            if (crashed || (exitCode.value_or(0) != 0 && treatAsError))
            {
              if (report.state != CompileTaskState::Failed)
              {
                failTask(*current);
              }
              addError(runnerMessage(line), lineNumber);
            }
            else if (!states[*current].hasOutcome)
            {
              succeed(*current);
            }
          }
          else if (crashed)
          {
            addError(runnerMessage(line), lineNumber);
          }
        }
        else if (startsWith(line, ProcessErrorPrefix))
        {
          if (current)
          {
            failTask(*current);
          }
          addError(runnerMessage(line), lineNumber);
        }
      }
      else if (startsWith(line, TerminatedPrefix))
      {
        result.terminated = true;
      }
      else if (startsWith(line, WorkDirFatalPrefix))
      {
        fatal = true;
        addError(runnerMessage(line), lineNumber);
      }
      else if (startsWith(line, WorkDirMissingPrefix))
      {
        addWarning(runnerMessage(line), lineNumber);
      }
      // "#### Using working directory" and unknown runner lines carry no information
      continue;
    }

    if (consumed[i])
    {
      continue;
    }

    // tool output
    const auto message = classifyToolLine(line);
    const auto leakLine = isLeakLine(line, message);
    if (leakLine && !result.leak)
    {
      result.leak = CompileLeak{};
      result.leak->line = lineNumber;
      result.leak->text = std::string{trim(line)};
      leakOpen = true;
    }
    if (result.leak && leakOpen)
    {
      addLeakDetails(*result.leak, line);
    }

    if (message.severity == Severity::None)
    {
      continue;
    }

    auto text = message.message;
    if (message.onNextLine)
    {
      // the message is on the next non-empty line, e.g. after "**** ERROR ****"
      for (auto j = i + 1; j < lines.size(); ++j)
      {
        const auto next = trim(lines[j].text);
        if (!next.empty())
        {
          if (
            !startsWith(next, "####")
            && classifyToolLine(next).severity == Severity::None)
          {
            text = std::string{trimStars(next)};
            consumed[j] = true;
          }
          break;
        }
      }
    }
    if (text.empty())
    {
      text = std::string{trim(line)};
    }

    if (message.severity == Severity::Error)
    {
      addError(std::move(text), lineNumber);
    }
    else
    {
      addWarning(std::move(text), lineNumber);
    }
  }

  // finish the task states
  const auto stopped = ended || result.terminated || fatal;
  const auto isFailed = [](const auto& report) {
    return report.state == CompileTaskState::Failed;
  };
  const auto anyFailed = std::ranges::any_of(result.tasks, isFailed);
  if (current && !states[*current].hasOutcome)
  {
    if (!stopped)
    {
      result.currentTask = current;
    }
    else if (result.terminated)
    {
      result.tasks[*current].state = CompileTaskState::Skipped;
    }
    else if (!anyFailed && *current + 1 == enabledTasks.size())
    {
      // the last task ended without an outcome line: a task that completes silently,
      // or a tool in a test run
      succeed(*current);
    }
    else
    {
      failTask(*current);
    }
  }

  const auto isSucceeded = [](const auto& report) {
    return report.state == CompileTaskState::Succeeded;
  };
  result.failed = fatal || std::ranges::any_of(result.tasks, isFailed);
  if (
    ended && !result.failed && !result.terminated
    && !std::ranges::all_of(result.tasks, isSucceeded))
  {
    // the run ended before all tasks were run
    result.failed = true;
  }

  if (stopped)
  {
    for (auto& report : result.tasks)
    {
      if (report.state == CompileTaskState::Pending)
      {
        report.state = CompileTaskState::Skipped;
      }
    }
  }

  result.completedTasks = size_t(std::ranges::count_if(result.tasks, isSucceeded));
  return result;
}

std::pair<std::string, bool> logTail(const std::string_view log, const size_t count)
{
  const auto lines = splitLines(log);
  if (lines.size() <= count)
  {
    return {std::string{log}, false};
  }
  if (count == 0)
  {
    return {std::string{}, true};
  }
  return {std::string{log.substr(lines[lines.size() - count].offset)}, true};
}

} // namespace tb::mcp
