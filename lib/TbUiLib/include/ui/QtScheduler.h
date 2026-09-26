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

#include <QObject>

#include "mcp/Scheduler.h"

namespace tb::ui
{

/**
 * Runs scheduled functions on the Qt event loop of the thread that owns the scheduler.
 * Functions that have not run yet when the scheduler is destroyed are discarded.
 */
class QtScheduler : public mcp::Scheduler
{
private:
  QObject m_context;

public:
  QtScheduler();
  ~QtScheduler() override;

  void post(std::function<void()> function) override;
  void postDelayed(
    std::chrono::milliseconds delay, std::function<void()> function) override;
  std::chrono::steady_clock::time_point now() const override;
};

} // namespace tb::ui
