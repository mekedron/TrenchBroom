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

#include <chrono>
#include <functional>

namespace tb::mcp
{

/**
 * Runs functions later on the thread that owns the MCP server (the Qt main thread in the
 * editor). Implemented by ui::QtScheduler and by FakeScheduler in tests.
 */
class Scheduler
{
public:
  virtual ~Scheduler();

  /**
   * Runs the given function as soon as possible, but not before the current call returns.
   */
  virtual void post(std::function<void()> function) = 0;

  /**
   * Runs the given function after the given delay.
   */
  virtual void postDelayed(
    std::chrono::milliseconds delay, std::function<void()> function) = 0;

  /**
   * Returns the current time.
   */
  virtual std::chrono::steady_clock::time_point now() const = 0;
};

} // namespace tb::mcp
