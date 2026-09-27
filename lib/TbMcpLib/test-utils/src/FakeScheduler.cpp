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

#include "mcp/FakeScheduler.h"

#include <algorithm>

namespace tb::mcp
{

FakeScheduler::FakeScheduler()
  : m_now{std::chrono::steady_clock::time_point{} + std::chrono::hours{1}}
{
}

void FakeScheduler::post(std::function<void()> function)
{
  m_tasks.push_back(Task{m_now, m_sequence++, std::move(function)});
}

void FakeScheduler::postDelayed(
  const std::chrono::milliseconds delay, std::function<void()> function)
{
  m_tasks.push_back(Task{m_now + delay, m_sequence++, std::move(function)});
}

std::chrono::steady_clock::time_point FakeScheduler::now() const
{
  return m_now;
}

size_t FakeScheduler::runPending()
{
  auto count = size_t{0};
  while (true)
  {
    const auto it =
      std::ranges::min_element(m_tasks, [](const auto& lhs, const auto& rhs) {
        return lhs.due < rhs.due || (lhs.due == rhs.due && lhs.sequence < rhs.sequence);
      });
    if (it == m_tasks.end() || it->due > m_now)
    {
      return count;
    }
    auto function = std::move(it->function);
    m_tasks.erase(it);
    function();
    ++count;
  }
}

void FakeScheduler::advance(const std::chrono::milliseconds duration)
{
  const auto target = m_now + duration;
  while (true)
  {
    runPending();
    const auto it = std::ranges::min_element(
      m_tasks, [](const auto& lhs, const auto& rhs) { return lhs.due < rhs.due; });
    if (it == m_tasks.end() || it->due > target)
    {
      break;
    }
    m_now = it->due;
  }
  m_now = target;
  runPending();
}

bool FakeScheduler::advanceToNextTask()
{
  const auto it = std::ranges::min_element(
    m_tasks, [](const auto& lhs, const auto& rhs) { return lhs.due < rhs.due; });
  if (it == m_tasks.end())
  {
    return false;
  }
  m_now = std::max(m_now, it->due);
  runPending();
  return true;
}

size_t FakeScheduler::pendingTaskCount() const
{
  return m_tasks.size();
}

} // namespace tb::mcp
