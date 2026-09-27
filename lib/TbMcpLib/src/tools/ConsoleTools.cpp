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


#include "mcp/tools/ConsoleTools.h"

#include "ToolUtils.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/ConsoleBuffer.h"
#include "mcp/LogCapture.h"
#include "mcp/Pagination.h"
#include "mcp/ResourceRegistry.h"
#include "mcp/ServerState.h"
#include "mcp/ToolRegistry.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tb::mcp
{
namespace
{
using namespace schema;

/** The most messages the console resource lists. */
constexpr size_t ResourceMessageCount = 200;

/** The value of console_read's document filter for messages without a document. */
constexpr auto NoDocument = std::string_view{"none"};

ToolError unsupportedError()
{
  return makeError(
    ErrorCode::UnsupportedInHost,
    "This host has no editor console.",
    "Console messages are only available in the TrenchBroom editor.");
}

std::optional<LogLevel> logLevelFromString(const std::string_view str)
{
  for (const auto level :
       {LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error})
  {
    if (toString(level) == str)
    {
      return level;
    }
  }
  return std::nullopt;
}

std::string toLower(const std::string_view str)
{
  auto result = std::string{str};
  std::ranges::transform(
    result, result.begin(), [](const unsigned char c) { return char(std::tolower(c)); });
  return result;
}

/** Maps the documents' addresses to their handles. */
std::unordered_map<const ui::MapDocument*, std::string> documentIds(McpHost& host)
{
  auto result = std::unordered_map<const ui::MapDocument*, std::string>{};
  for (const auto& document : host.documents())
  {
    result[document.document] = document.id;
  }
  return result;
}

/**
 * Describes a message like the per-call console report: `document` is the handle of the
 * message's document if it is open, otherwise `documentName` is its file name.
 */
Json messageJson(
  const ConsoleMessage& message,
  const std::unordered_map<const ui::MapDocument*, std::string>& documentIds)
{
  auto json = Json{
    {"seq", message.seq},
    {"level", toString(message.level)},
    {"time", isoTime(message.time)},
    {"text", message.text},
  };
  if (const auto it = documentIds.find(message.document); it != documentIds.end())
  {
    json["document"] = it->second;
  }
  else if (!message.documentName.empty())
  {
    json["documentName"] = message.documentName;
  }
  return json;
}

/** Matches the messages against console_read's filters. */
class MessageFilter
{
private:
  LogLevel m_minLevel = LogLevel::Info;
  std::optional<std::string> m_text;
  std::optional<std::regex> m_regex;
  std::optional<std::string> m_documentFilter;
  const ui::MapDocument* m_document = nullptr;

public:
  static Result<MessageFilter, ToolError> create(CallContext& context, const Args& args)
  {
    auto filter = MessageFilter{};
    filter.m_minLevel = *logLevelFromString(args.get<std::string>("minLevel"));

    if (const auto text = args.getOptional<std::string>("text"); text && !text->empty())
    {
      if (args.get<bool>("regex"))
      {
        try
        {
          filter.m_regex = std::regex{*text, std::regex::ECMAScript | std::regex::icase};
        }
        catch (const std::regex_error& e)
        {
          return makeError(
            ErrorCode::InvalidArgument,
            fmt::format("'text' is not a valid regular expression: {}", e.what()),
            "Pass an ECMAScript regular expression, or set 'regex' to false to search "
            "for the text as it is.");
        }
      }
      else
      {
        filter.m_text = toLower(*text);
      }
    }

    if (const auto document = args.getOptional<std::string>("document"))
    {
      if (document->starts_with("doc:"))
      {
        const auto info = context.server().findDocument(*document);
        if (!info)
        {
          return makeError(
            ErrorCode::DocumentNotFound,
            fmt::format("Document {} is not open.", *document),
            "Use document_list to see the open documents, or pass the file name of a "
            "closed document.");
        }
        filter.m_document = info->document;
      }
      filter.m_documentFilter = *document;
    }

    return filter;
  }

  bool matches(
    const ConsoleMessage& message,
    const std::unordered_map<const ui::MapDocument*, std::string>& documentIds) const
  {
    if (message.level < m_minLevel)
    {
      return false;
    }
    if (m_text && toLower(message.text).find(*m_text) == std::string::npos)
    {
      return false;
    }
    if (m_regex && !std::regex_search(message.text, *m_regex))
    {
      return false;
    }
    if (m_documentFilter)
    {
      if (m_document)
      {
        return message.document == m_document;
      }
      const auto open = documentIds.contains(message.document);
      if (*m_documentFilter == NoDocument)
      {
        return !open && message.documentName.empty();
      }
      return !open && message.documentName == *m_documentFilter;
    }
    return true;
  }
};

ToolResult consoleRead(CallContext& context, const Args& args)
{
  const auto* buffer = context.host().consoleBuffer();
  if (!buffer)
  {
    return unsupportedError();
  }

  auto filter = MessageFilter::create(context, args);
  if (filter.is_error())
  {
    return errorOf(filter);
  }

  // the messages after this sequence number are requested
  auto after = std::optional<uint64_t>{};
  if (const auto cursor = args.getOptional<std::string>("cursor"))
  {
    const auto decoded = decodeCursor(*cursor);
    if (!decoded)
    {
      return makeError(
        ErrorCode::InvalidArgument,
        "The cursor is invalid.",
        "Pass the nextCursor value of the previous page unchanged, or use 'after'.");
    }
    after = uint64_t(decoded->offset);
  }
  else if (const auto afterArg = args.getOptional<int64_t>("after"))
  {
    after = uint64_t(*afterArg);
  }

  const auto limit = size_t(args.get<int64_t>("limit"));
  const auto newest = args.get<bool>("newest");
  const auto ids = documentIds(context.host());

  auto matching = std::vector<const ConsoleMessage*>{};
  for (const auto& message : buffer->messages())
  {
    if (message.seq > after.value_or(0) && filter.value().matches(message, ids))
    {
      matching.push_back(&message);
    }
  }

  const auto count = std::min(limit, matching.size());
  const auto first = newest ? matching.size() - count : size_t{0};
  auto items = Json::array();
  for (auto i = first; i < first + count; ++i)
  {
    items.push_back(messageJson(*matching[i], ids));
  }

  const auto more = !newest && count < matching.size();
  const auto lastSeq = more ? matching[count - 1]->seq : buffer->lastSeq();
  auto result = Json{
    {"items", std::move(items)},
    {"total", matching.size()},
    {"nextCursor", more ? Json(encodeCursor(size_t(lastSeq), 0)) : Json(nullptr)},
    {"lastSeq", lastSeq},
  };

  // messages after the requested position that are gone (buffer limit or cleared)
  if (after)
  {
    const auto oldest =
      buffer->messages().empty() ? buffer->lastSeq() + 1 : buffer->messages().front().seq;
    if (oldest > *after + 1)
    {
      result["dropped"] = oldest - *after - 1;
    }
  }
  return result;
}

ToolResult consoleClear(CallContext& context, const Args&)
{
  auto* buffer = context.host().consoleBuffer();
  if (!buffer)
  {
    return unsupportedError();
  }

  const auto count = buffer->messages().size();
  if (context.dryRun())
  {
    return Json{
      {"wouldDo",
       fmt::format("clear {} buffered messages and the editor's console views", count)},
      {"cleared", count},
      {"lastSeq", buffer->lastSeq()},
    };
  }

  buffer->clear();
  context.host().clearConsoleViews();
  return Json{
    {"cleared", count},
    {"lastSeq", buffer->lastSeq()},
  };
}

Schema messageSchema()
{
  return object({
    field("seq", integer()).required(),
    field("level", enumOf({"debug", "info", "warning", "error"})).required(),
    field("time", string()).required(),
    field("text", string()).required(),
    field("document", string()),
    field("documentName", string()),
  });
}

} // namespace

void registerConsoleTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"console_read"}
      .title("Read Console")
      .description(
        "Returns the messages of the editor console, oldest first: everything the editor "
        "logs (material, model and entity definition loading problems, invalid map data, "
        "compile and save messages, the agent call log lines '[AI] ...'), not only "
        "messages caused by agent calls. Each message has seq, level (debug, info, "
        "warning, error), time, text and its document ('document' if it is open, else "
        "'documentName'). Filters: minLevel (default 'info'), text (case-insensitive "
        "substring, or a regular expression with regex: true), document ('doc:<n>', the "
        "file name of a closed document, or 'none'). To read only new messages, pass the "
        "previous result's lastSeq as 'after'; 'dropped' counts messages after it that "
        "are no longer buffered (buffer limit or console_clear). Pages continue with "
        "nextCursor; newest: true returns the newest 'limit' messages instead. "
        "Example: {\"minLevel\": \"warning\", \"after\": 120}")
      .input(object({
        field(
          "minLevel", enumOf({"debug", "info", "warning", "error"}).defaultsTo("info"))
          .describe("Only messages of this level or higher"),
        field("text", string())
          .describe("Only messages that contain this text (case-insensitive)"),
        field("regex", boolean().defaultsTo(false))
          .describe("Treat 'text' as a case-insensitive ECMAScript regular expression"),
        field("document", string())
          .describe(
            "Only messages of this document: a handle such as 'doc:1', the file name of "
            "a "
            "closed document, or 'none' for messages without a document. Default: all "
            "messages"),
        field("after", integer().min(0))
          .describe("Only messages with a greater seq, e.g. the lastSeq of the previous "
                    "read"),
        field("newest", boolean().defaultsTo(false))
          .describe("Return the newest matching messages instead of the oldest"),
        field("cursor", string()).describe("Opaque cursor from a previous nextCursor"),
        field("limit", integer().min(1).max(1000).defaultsTo(100))
          .describe("Maximum number of messages to return"),
      }))
      .output(object({
        field("items", array(messageSchema())).required(),
        field("total", integer())
          .required()
          .describe("The number of matching messages, including other pages"),
        field("nextCursor", any()).describe("Cursor of the next page, or null"),
        field("lastSeq", integer())
          .required()
          .describe("Pass as 'after' to read only newer messages next time"),
        field("dropped", integer())
          .describe("Messages after 'after' or the cursor that are no longer buffered"),
      }))
      .mutation(Mutation::None)
      .handler(consoleRead));

  registry.add(
    ToolDef{"console_clear"}
      .title("Clear Console")
      .description(
        "Clears the buffered console messages that console_read returns and the console "
        "views of the editor's windows. Not undoable. Sequence numbers continue, so a "
        "later console_read with 'after' reports the cleared messages as dropped. "
        "Example: {}")
      .input(object({}))
      .output(object({
        field("cleared", integer())
          .required()
          .describe("The number of buffered messages removed (or that would be)"),
        field("lastSeq", integer()).required(),
        field("wouldDo", string()),
      }))
      .mutation(Mutation::External)
      .destructive()
      .idempotent()
      .handler(consoleClear));
}

void registerConsoleResources(ResourceRegistry& registry)
{
  registry.add(ResourceDef{
    ConsoleResourceUri,
    "console",
    "Editor Console",
    fmt::format(
      "The newest {} console messages of level info or higher, oldest first, with "
      "lastSeq (use console_read with 'after' for more). Subscribe to get notified when "
      "new messages arrive; bursts of messages are coalesced into one notification, and "
      "the agent call log lines '[AI] ...' do not notify.",
      ResourceMessageCount),
    "application/json",
    [](ServerState& state, Session&, const std::string& uri, const auto&)
      -> Result<Json, ToolError> {
      const auto* buffer = state.host.consoleBuffer();
      if (!buffer)
      {
        return unsupportedError();
      }

      const auto ids = documentIds(state.host);
      auto messages = std::vector<Json>{};
      for (auto it = buffer->messages().rbegin();
           it != buffer->messages().rend() && messages.size() < ResourceMessageCount;
           ++it)
      {
        if (it->level >= LogLevel::Info)
        {
          messages.push_back(messageJson(*it, ids));
        }
      }
      std::ranges::reverse(messages);

      return jsonResourceContents(
        uri,
        Json{
          {"messages", std::move(messages)},
          {"lastSeq", buffer->lastSeq()},
          {"buffered", buffer->messages().size()},
          {"capacity", buffer->capacity()},
        });
    },
  });
}

} // namespace tb::mcp
