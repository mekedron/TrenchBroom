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

#pragma once

#include "mcp/Scheduler.h"

#include <chrono>
#include <functional>
#include <vector>

namespace tb::mcp
{

/**
 * A scheduler for tests. Nothing runs until the test calls `runPending` or `advance`.
 */
class FakeScheduler : public Scheduler
{
private:
  struct Task
  {
    std::chrono::steady_clock::time_point due;
    size_t sequence;
    std::function<void()> function;
  };

  std::chrono::steady_clock::time_point m_now;
  size_t m_sequence = 0;
  std::vector<Task> m_tasks;

public:
  FakeScheduler();

  void post(std::function<void()> function) override;
  void postDelayed(
    std::chrono::milliseconds delay, std::function<void()> function) override;
  std::chrono::steady_clock::time_point now() const override;

  /**
   * Runs all tasks that are due now, including tasks posted by those tasks. Returns the
   * number of tasks run.
   */
  size_t runPending();

  /**
   * Advances the clock by the given duration, running every task that becomes due in
   * order.
   */
  void advance(std::chrono::milliseconds duration);

  size_t pendingTaskCount() const;
};

} // namespace tb::mcp
