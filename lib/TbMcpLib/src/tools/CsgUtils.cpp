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

#include "mcp/tools/CsgUtils.h"

#include "base/Logger.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Selection.h"
#include "mdl/Transaction.h"
#include "mdl/WorldNode.h"

#include "kd/flat_map.h"
#include "kd/ranges/as_rvalue_view.h"
#include "kd/ranges/to.h"
#include "kd/result_fold.h"
#include "kd/vector_utils.h"

#include <ranges>
#include <vector>

namespace tb::mcp
{

bool csgHollow(mdl::Map& map, const double thickness)
{
  const auto brushNodes = map.selection().brushes;
  if (brushNodes.empty())
  {
    return false;
  }

  if (thickness <= 0.0)
  {
    map.logger().error() << "Could not hollow brushes: thickness must be positive";
    return false;
  }

  bool didHollowAnything = false;
  auto toAdd = kdl::flat_map<mdl::Node*, std::vector<mdl::Node*>>{};
  auto toRemove = std::vector<mdl::Node*>{};

  for (auto* brushNode : brushNodes)
  {
    const auto& originalBrush = brushNode->brush();

    auto shrunkenBrush = originalBrush;
    shrunkenBrush.expand(map.worldBounds(), -thickness, true) | kdl::and_then([&]() {
      didHollowAnything = true;

      return originalBrush.subtract(
               map.worldNode().mapFormat(),
               map.worldBounds(),
               map.currentMaterialName(),
               shrunkenBrush)
             | kdl::fold | kdl::transform([&](auto fragments) {
                 auto fragmentNodes =
                   fragments | kdl::views::as_rvalue
                   | std::views::transform([](auto&& b) {
                       return new mdl::BrushNode{std::forward<decltype(b)>(b)};
                     })
                   | kdl::ranges::to<std::vector>();

                 auto& toAddForParent = toAdd[brushNode->parent()];
                 kdl::vec_append(toAddForParent, fragmentNodes);
                 toRemove.push_back(brushNode);
               });
    }) | kdl::transform_error([&](const auto& e) {
      map.logger().error() << "Could not hollow brush: " << e;
    });
  }

  if (!didHollowAnything)
  {
    return false;
  }

  auto transaction = mdl::Transaction{map, "CSG Hollow"};
  mdl::deselectAll(map);
  const auto added = mdl::addNodes(map, toAdd);
  if (added.empty())
  {
    transaction.cancel();
    return false;
  }
  mdl::removeNodes(map, toRemove);
  mdl::selectNodes(map, added);

  return transaction.commit();
}

} // namespace tb::mcp
