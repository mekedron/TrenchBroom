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

#include "mdl/CommandProcessor.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/TransactionScope.h"
#include "mdl/UndoableCommand.h"

#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mcp
{
namespace
{

class NullCommand : public mdl::UndoableCommand
{
public:
  explicit NullCommand(std::string name)
    : UndoableCommand{std::move(name), true}
  {
  }

  bool doPerformDo(mdl::Map&) override { return true; }

  bool doPerformUndo(mdl::Map&) override { return true; }
};

} // namespace

// Tests the additions to mdl::CommandProcessor that the MCP server relies on.
TEST_CASE("UpstreamCommandProcessor")
{
  using mdl::TransactionScope;

  auto fixture = mdl::MapFixture{};
  auto& map = fixture.create();

  auto commandProcessor = mdl::CommandProcessor{map};

  SECTION("transactionDepth")
  {
    CHECK(commandProcessor.transactionDepth() == 0u);

    commandProcessor.startTransaction("outer", TransactionScope::LongRunning);
    CHECK(commandProcessor.transactionDepth() == 1u);

    commandProcessor.startTransaction("inner", TransactionScope::Oneshot);
    CHECK(commandProcessor.transactionDepth() == 2u);

    commandProcessor.commitTransaction();
    CHECK(commandProcessor.transactionDepth() == 1u);

    commandProcessor.rollbackTransaction();
    commandProcessor.commitTransaction();
    CHECK(commandProcessor.transactionDepth() == 0u);
  }

  SECTION("undoCommandNames")
  {
    CHECK(commandProcessor.undoCommandNames().empty());

    commandProcessor.executeAndStore(std::make_unique<NullCommand>("command 1"));
    commandProcessor.executeAndStore(std::make_unique<NullCommand>("command 2"));
    CHECK(
      commandProcessor.undoCommandNames()
      == std::vector<std::string>{"command 2", "command 1"});

    REQUIRE(commandProcessor.undo());
    CHECK(commandProcessor.undoCommandNames() == std::vector<std::string>{"command 1"});
  }

  SECTION("redoCommandNames")
  {
    commandProcessor.executeAndStore(std::make_unique<NullCommand>("command 1"));
    commandProcessor.executeAndStore(std::make_unique<NullCommand>("command 2"));
    CHECK(commandProcessor.redoCommandNames().empty());

    REQUIRE(commandProcessor.undo());
    CHECK(commandProcessor.redoCommandNames() == std::vector<std::string>{"command 2"});

    REQUIRE(commandProcessor.undo());
    CHECK(
      commandProcessor.redoCommandNames()
      == std::vector<std::string>{"command 1", "command 2"});
  }

  SECTION("redo stack")
  {
    commandProcessor.executeAndStore(std::make_unique<NullCommand>("command 1"));
    commandProcessor.executeAndStore(std::make_unique<NullCommand>("command 2"));
    REQUIRE(commandProcessor.undo());
    REQUIRE(commandProcessor.canRedo());

    SECTION("is preserved when a transaction is rolled back")
    {
      commandProcessor.startTransaction("transaction", TransactionScope::Oneshot);
      commandProcessor.executeAndStore(std::make_unique<NullCommand>("command 3"));
      commandProcessor.rollbackTransaction();
      commandProcessor.commitTransaction();

      REQUIRE(commandProcessor.canRedo());
      CHECK(*commandProcessor.redoCommandName() == "command 2");
    }

    SECTION(
      "is preserved when a nested transaction is committed and the outer one is "
      "rolled back")
    {
      commandProcessor.startTransaction("outer", TransactionScope::LongRunning);
      commandProcessor.startTransaction("inner", TransactionScope::Oneshot);
      commandProcessor.executeAndStore(std::make_unique<NullCommand>("command 3"));
      commandProcessor.commitTransaction();
      commandProcessor.rollbackTransaction();
      commandProcessor.commitTransaction();

      REQUIRE(commandProcessor.canRedo());
      CHECK(*commandProcessor.redoCommandName() == "command 2");
    }

    SECTION("is cleared when a transaction is committed")
    {
      commandProcessor.startTransaction("transaction", TransactionScope::Oneshot);
      commandProcessor.executeAndStore(std::make_unique<NullCommand>("command 3"));
      commandProcessor.commitTransaction();

      CHECK(!commandProcessor.canRedo());
    }

    SECTION("is cleared when a command is stored outside of a transaction")
    {
      commandProcessor.executeAndStore(std::make_unique<NullCommand>("command 3"));

      CHECK(!commandProcessor.canRedo());
    }

    SECTION("is preserved by redo")
    {
      REQUIRE(commandProcessor.undo());
      REQUIRE(commandProcessor.redo());

      REQUIRE(commandProcessor.canRedo());
      CHECK(*commandProcessor.redoCommandName() == "command 2");
    }
  }
}

} // namespace tb::mcp
