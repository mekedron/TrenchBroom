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

namespace tb::mdl
{
class Map;
}

namespace tb::mcp
{

/**
 * Hollows the selected brushes like mdl::csgHollow, but with walls of the given thickness
 * instead of the grid size. Logs an error and returns false if the thickness is not
 * positive.
 */
bool csgHollow(mdl::Map& map, double thickness);

} // namespace tb::mcp
