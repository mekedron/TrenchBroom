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

#include "mcp/CompileRuns.h"

#include "mcp/Host.h"
#include "mcp/tools/CompileUtils.h"
#include "mdl/Map.h"
#include "ui/MapDocument.h"

#include <algorithm>

namespace tb::mcp
{

CompileRun::~CompileRun() = default;

bool CompileRun::running() const
{
  return !endedAt.has_value();
}

std::string CompileRun::log() const
{
  return job ? job->log() : finalLog;
}

CompileRuns::CompileRuns(McpHost& host, DidChange didChange)
  : m_didChange{std::move(didChange)}
{
  m_notifierConnection +=
    host.documentWillCloseNotifier.connect(this, &CompileRuns::documentWillClose);
}

CompileRuns::~CompileRuns()
{
  *m_alive = false;
  // destroying the jobs terminates running compilations without calling back
  m_runs.clear();
}

Result<CompileRun*> CompileRuns::start(
  CompileHost& compileHost, ui::MapDocument& document, CompileRunSpec spec)
{
  const auto number = m_nextNumber++;

  auto run = std::make_unique<CompileRun>();
  run->number = number;
  run->id = "run:" + std::to_string(number);
  run->gameName = std::move(spec.gameName);
  run->documentId = std::move(spec.documentId);
  run->document = &document;
  run->mapPath = document.map().path();
  run->sessionId = std::move(spec.sessionId);
  run->tasks = enabledTasks(spec.profile);
  run->profile = std::move(spec.profile);
  run->preset = std::move(spec.preset);
  run->test = spec.test;
  run->startedAt = std::chrono::system_clock::now();

  // the run must be known before the job starts, because the job may end right away
  auto* runPtr = run.get();
  m_runs.push_back(std::move(run));

  const auto alive = std::weak_ptr<bool>{m_alive};
  auto callbacks = CompileJobCallbacks{
    [this, alive, number]() {
      if (const auto isAlive = alive.lock(); isAlive && *isAlive)
      {
        outputChanged(number);
      }
    },
    [this, alive, number]() {
      if (const auto isAlive = alive.lock(); isAlive && *isAlive)
      {
        jobEnded(number);
      }
    },
  };

  auto job = compileHost.startCompile(document, runPtr->profile, runPtr->test, callbacks);
  if (job.is_error())
  {
    std::erase_if(m_runs, [&](const auto& r) { return r.get() == runPtr; });
    return std::get<Error>(job.error());
  }

  runPtr->job = std::move(job).value();
  dropOldRuns();
  return runPtr;
}

void CompileRuns::cancel(CompileRun& run)
{
  if (run.running())
  {
    run.cancelRequested = true;
    if (run.job)
    {
      run.job->cancel();
    }
  }
}

CompileRun* CompileRuns::find(const std::string_view id) const
{
  const auto it =
    std::ranges::find_if(m_runs, [&](const auto& run) { return run->id == id; });
  return it != m_runs.end() ? it->get() : nullptr;
}

CompileRun* CompileRuns::latest(const ui::MapDocument* document) const
{
  for (auto it = m_runs.rbegin(); it != m_runs.rend(); ++it)
  {
    if (!document || (*it)->document == document)
    {
      return it->get();
    }
  }
  return nullptr;
}

CompileRun* CompileRuns::running(const ui::MapDocument& document) const
{
  const auto it = std::ranges::find_if(m_runs, [&](const auto& run) {
    return run->document == &document && run->running();
  });
  return it != m_runs.end() ? it->get() : nullptr;
}

std::vector<const CompileRun*> CompileRuns::runs() const
{
  auto result = std::vector<const CompileRun*>{};
  for (const auto& run : m_runs)
  {
    result.push_back(run.get());
  }
  return result;
}

std::string CompileRuns::logUri(const std::string_view runId)
{
  return "trenchbroom://compile/" + std::string{runId} + "/log";
}

CompileRun* CompileRuns::findByNumber(const size_t number) const
{
  const auto it =
    std::ranges::find_if(m_runs, [&](const auto& run) { return run->number == number; });
  return it != m_runs.end() ? it->get() : nullptr;
}

void CompileRuns::outputChanged(const size_t number)
{
  if (auto* run = findByNumber(number); run && run->running() && m_didChange)
  {
    m_didChange(*run, false);
  }
}

void CompileRuns::jobEnded(const size_t number)
{
  auto* run = findByNumber(number);
  if (!run || !run->running())
  {
    return;
  }

  run->endedAt = std::chrono::system_clock::now();
  if (run->job)
  {
    run->finalLog = run->job->log();
  }
  if (m_didChange)
  {
    m_didChange(*run, true);
  }
}

void CompileRuns::documentWillClose(ui::MapDocument& document)
{
  // index based: callbacks may run while the jobs are cancelled
  for (size_t i = 0; i < m_runs.size(); ++i)
  {
    auto& run = *m_runs[i];
    if (run.document != &document)
    {
      continue;
    }

    if (run.running())
    {
      run.documentClosed = true;
      if (run.job)
      {
        run.job->cancel();
      }
    }

    // the job refers to the document, so it must go now
    if (run.job)
    {
      run.finalLog = run.job->log();
      run.job.reset();
    }
    run.document = nullptr;

    if (run.running())
    {
      run.endedAt = std::chrono::system_clock::now();
      if (m_didChange)
      {
        m_didChange(run, true);
      }
    }
  }
}

void CompileRuns::dropOldRuns()
{
  auto ended =
    std::ranges::count_if(m_runs, [](const auto& run) { return !run->running(); });
  for (auto it = m_runs.begin(); it != m_runs.end() && size_t(ended) > MaxEndedRuns;)
  {
    if (!(*it)->running())
    {
      it = m_runs.erase(it);
      --ended;
    }
    else
    {
      ++it;
    }
  }
}

} // namespace tb::mcp
