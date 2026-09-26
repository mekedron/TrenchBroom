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

#include "ui/QtScheduler.h"

#include <QTimer>

namespace tb::ui
{

QtScheduler::QtScheduler() = default;

QtScheduler::~QtScheduler() = default;

void QtScheduler::post(std::function<void()> function)
{
  postDelayed(std::chrono::milliseconds{0}, std::move(function));
}

void QtScheduler::postDelayed(
  const std::chrono::milliseconds delay, std::function<void()> function)
{
  QTimer::singleShot(
    delay, &m_context, [function = std::move(function)]() { function(); });
}

std::chrono::steady_clock::time_point QtScheduler::now() const
{
  return std::chrono::steady_clock::now();
}

} // namespace tb::ui
