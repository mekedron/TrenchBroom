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

#include "mcp/tools/KnowledgeTools.h"

#include "ToolUtils.h"
#include "base/PreferenceManager.h"
#include "mcp/Args.h"
#include "mcp/CallContext.h"
#include "mcp/Pagination.h"
#include "mcp/ResourceRegistry.h"
#include "mcp/ServerState.h"
#include "mcp/ToolRegistry.h"
#include "mcp/tools/Manual.h"
#include "mcp/tools/PreferenceCatalog.h"

#include "kd/string_format.h"
#include "kd/string_utils.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace tb::mcp
{
namespace
{
using namespace schema;

constexpr auto ManualUri = "trenchbroom://manual";
constexpr auto ManualSectionUriTemplate = "trenchbroom://manual/{section}";

std::string sectionUri(const ManualSection& section)
{
  return fmt::format("{}/{}", ManualUri, percentEncode(section.id));
}

ToolError noManualError()
{
  return makeError(
    ErrorCode::UnsupportedInHost,
    "This host has no user manual.",
    "The manual is available in the TrenchBroom editor when its manual was built "
    "(manual/index.html in the editor's resources).");
}

Result<std::shared_ptr<const LoadedManual>, ToolError> manualOf(McpHost& host)
{
  const auto path = host.manualPath();
  if (!path)
  {
    return noManualError();
  }
  auto manual = loadManual(*path);
  if (manual.is_error())
  {
    return makeError(
      ErrorCode::IoError,
      fmt::format("The user manual cannot be read: {}", errorMessage(manual)),
      "Check that manual/index.html exists in the editor's resources.");
  }
  return manual.value();
}

// Shortcut references

/**
 * Resolves the manual's references to menu items, actions and keys: the menu paths and
 * shortcuts of the document's window if there is one (the action host), the current
 * shortcut preferences, and the manual's own shortcut table (the defaults at build time).
 */
class ReferenceResolver
{
private:
  std::unordered_map<std::string, EditorAction> m_actions;
  std::unordered_map<std::string, std::vector<std::string>> m_preferenceShortcuts;
  const ManualShortcuts* m_table = nullptr;

public:
  ReferenceResolver(McpHost& host, ui::MapDocument* document, const LoadedManual& manual)
    : m_table{manual.shortcuts ? &*manual.shortcuts : nullptr}
  {
    if (document)
    {
      if (auto* actionHost = host.actionHost())
      {
        if (auto actions = actionHost->actions(*document, std::nullopt);
            actions.is_success())
        {
          for (auto& action : actions.value())
          {
            auto path = action.path;
            m_actions.emplace(std::move(path), std::move(action));
          }
        }
      }
    }

    for (const auto& info : allPreferences(host, document))
    {
      if (
        const auto* preference =
          std::get_if<Preference<std::vector<KeySequence>>*>(&info.preference))
      {
        auto shortcuts = std::vector<std::string>{};
        for (const auto& keySequence : pref(**preference))
        {
          if (!keySequence.value.empty())
          {
            shortcuts.push_back(keySequence.value);
          }
        }
        m_preferenceShortcuts.emplace(info.path().generic_string(), std::move(shortcuts));
      }
    }
  }

  std::string operator()(const ManualReference kind, const std::string& key) const
  {
    switch (kind)
    {
    case ManualReference::MenuItem:
      return menuItem(key);
    case ManualReference::Action:
      return action(key);
    case ManualReference::Key:
      return keyName(key);
    }
    return key;
  }

private:
  static std::string joinShortcuts(const std::vector<std::string>& shortcuts)
  {
    return kdl::str_join(shortcuts, " or ");
  }

  static std::string lastComponent(const std::string& path)
  {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
  }

  std::optional<std::vector<std::string>> shortcuts(const std::string& key) const
  {
    if (const auto it = m_actions.find(key); it != m_actions.end())
    {
      return it->second.shortcuts;
    }
    if (const auto it = m_preferenceShortcuts.find(key);
        it != m_preferenceShortcuts.end())
    {
      return it->second;
    }
    if (m_table)
    {
      if (const auto it = m_table->menu.find(key); it != m_table->menu.end())
      {
        return it->second.shortcuts;
      }
      if (const auto it = m_table->actions.find(key); it != m_table->actions.end())
      {
        return it->second;
      }
    }
    return std::nullopt;
  }

  const ManualShortcuts::MenuItem* tableMenuItem(const std::string& key) const
  {
    if (m_table)
    {
      if (const auto it = m_table->menu.find(key); it != m_table->menu.end())
      {
        return &it->second;
      }
    }
    return nullptr;
  }

  std::string menuItem(const std::string& key) const
  {
    auto path = std::vector<std::string>{};
    if (const auto it = m_actions.find(key); it != m_actions.end())
    {
      path = it->second.menu;
      path.push_back(it->second.label);
    }
    else if (const auto* item = tableMenuItem(key))
    {
      path = item->path;
    }
    else
    {
      path = kdl::str_split(key.starts_with("Menu/") ? key.substr(5) : key, "/");
    }

    auto result = fmt::format("**{}**", kdl::str_join(path, " > "));
    if (const auto keys = shortcuts(key); keys && !keys->empty())
    {
      result += fmt::format(" ({})", joinShortcuts(*keys));
    }
    return result;
  }

  std::string action(const std::string& key) const
  {
    if (const auto keys = shortcuts(key); keys && !keys->empty())
    {
      return fmt::format("**{}**", joinShortcuts(*keys));
    }
    return fmt::format("**{}** (no shortcut)", lastComponent(key));
  }

  std::string keyName(const std::string& key) const
  {
    if (m_table)
    {
      if (const auto it = m_table->keys.find(key); it != m_table->keys.end())
      {
        return fmt::format("**{}**", it->second);
      }
    }
    return fmt::format("**{}**", key);
  }
};

/** The sections' texts with resolved references. */
std::vector<std::string> resolvedTexts(
  const Manual& manual, const ReferenceResolver& resolver)
{
  auto result = std::vector<std::string>{};
  result.reserve(manual.sections.size());
  for (const auto& section : manual.sections)
  {
    result.push_back(resolveManualText(section.text, std::cref(resolver)));
  }
  return result;
}

// Search

/** The positions of the word in the lower case text; "'" also matches "\u2019". */
std::vector<size_t> findWord(const std::string_view text, const std::string& word)
{
  auto variants = std::vector<std::string>{word};
  if (word.find('\'') != std::string::npos)
  {
    variants.push_back(kdl::str_replace_every(word, "'", "\u2019"));
  }

  auto result = std::vector<size_t>{};
  for (const auto& variant : variants)
  {
    for (auto pos = text.find(variant); pos != std::string_view::npos;
         pos = text.find(variant, pos + variant.size()))
    {
      result.push_back(pos);
    }
  }
  std::ranges::sort(result);
  return result;
}

bool isContinuationByte(const char c)
{
  return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

size_t alignStart(const std::string_view text, size_t pos)
{
  while (pos < text.size() && isContinuationByte(text[pos]))
  {
    ++pos;
  }
  return pos;
}

size_t alignEnd(const std::string_view text, size_t pos)
{
  while (pos > 0 && pos < text.size() && isContinuationByte(text[pos]))
  {
    --pos;
  }
  return pos;
}

std::string snippet(const std::string_view text, const size_t pos, const size_t length)
{
  constexpr auto Before = size_t{70};
  constexpr auto After = size_t{110};

  auto start = pos > Before ? pos - Before : 0;
  auto end = std::min(text.size(), pos + length + After);

  // start and end at word boundaries
  if (start > 0)
  {
    if (const auto space = text.find_first_of(" \n", start);
        space != std::string_view::npos && space < pos)
    {
      start = space + 1;
    }
  }
  if (end < text.size())
  {
    if (const auto space = text.find_last_of(" \n", end);
        space != std::string_view::npos && space > pos + length)
    {
      end = space;
    }
  }
  start = alignStart(text, start);
  end = alignEnd(text, end);

  auto result = std::string{};
  auto space = false;
  for (const auto c : text.substr(start, end - start))
  {
    if (c == ' ' || c == '\n' || c == '\t')
    {
      space = !result.empty();
    }
    else
    {
      if (space)
      {
        result += ' ';
        space = false;
      }
      result += c;
    }
  }
  return fmt::format(
    "{}{}{}", start > 0 ? "\u2026" : "", result, end < text.size() ? "\u2026" : "");
}

struct SearchHit
{
  size_t index = 0;
  double score = 0.0;
  std::vector<std::string> snippets;
};

std::vector<SearchHit> search(
  const Manual& manual, const std::vector<std::string>& texts, const std::string& query)
{
  const auto lowerQuery = kdl::str_to_lower(kdl::str_trim(query));
  const auto words = kdl::str_split(lowerQuery, " \t\n");
  if (words.empty())
  {
    return {};
  }

  auto hits = std::vector<SearchHit>{};
  for (size_t i = 0; i < manual.sections.size(); ++i)
  {
    const auto& section = manual.sections[i];
    const auto title = kdl::str_to_lower(section.title);
    const auto text = kdl::str_to_lower(texts[i]);

    auto score = 0.0;
    auto matched = size_t{0};
    auto positions = std::vector<std::pair<size_t, size_t>>{};
    for (const auto& word : words)
    {
      const auto titleHits = findWord(title, word).size();
      const auto textHits = findWord(text, word);
      if (titleHits == 0 && textHits.empty())
      {
        continue;
      }
      ++matched;
      score += 10.0 * double(titleHits) + std::min(double(textHits.size()), 10.0);
      if (!textHits.empty())
      {
        positions.emplace_back(textHits.front(), word.size());
      }
    }
    if (matched == 0)
    {
      continue;
    }

    // sections that contain all words rank first; phrases and titles give a bonus
    const auto coverage = double(matched) / double(words.size());
    score *= coverage * coverage;
    if (words.size() > 1)
    {
      if (title.find(lowerQuery) != std::string::npos)
      {
        score += 50.0;
      }
      else if (text.find(lowerQuery) != std::string::npos)
      {
        score += 20.0;
      }
    }
    if (title == lowerQuery)
    {
      score += 50.0;
    }

    auto hit = SearchHit{i, std::round(score * 10.0) / 10.0, {}};
    std::ranges::sort(positions);
    auto lastEnd = std::optional<size_t>{};
    for (const auto& [pos, length] : positions)
    {
      if (hit.snippets.size() == 2)
      {
        break;
      }
      if (lastEnd && pos < *lastEnd)
      {
        continue;
      }
      hit.snippets.push_back(snippet(texts[i], pos, length));
      lastEnd = pos + length + 110;
    }
    hits.push_back(std::move(hit));
  }

  std::ranges::stable_sort(
    hits, [](const auto& lhs, const auto& rhs) { return lhs.score > rhs.score; });
  return hits;
}

Json sectionRef(const Manual& manual, const std::optional<size_t> index)
{
  if (!index || *index >= manual.sections.size())
  {
    return nullptr;
  }
  const auto& section = manual.sections[*index];
  return Json{{"id", section.id}, {"title", section.title}};
}

ToolError unknownSectionError(
  const Manual& manual, const std::vector<std::string>& texts, const std::string& name)
{
  auto suggestions = std::vector<std::string>{};
  for (const auto& hit : search(
         manual,
         texts,
         kdl::str_replace_every(kdl::str_replace_every(name, "_", " "), "-", " ")))
  {
    if (suggestions.size() == 3)
    {
      break;
    }
    const auto& section = manual.sections[hit.index];
    suggestions.push_back(fmt::format("'{}' ({})", section.id, section.title));
  }
  return makeError(
    ErrorCode::ObjectNotFound,
    fmt::format("The manual has no section '{}'.", name),
    suggestions.empty()
      ? "Use manual_search to find sections, or read trenchbroom://manual for the table "
        "of contents."
      : fmt::format(
          "Did you mean {}? Use manual_search to find sections.",
          kdl::str_join(suggestions, ", ")));
}

ui::MapDocument* documentOf(CallContext& context)
{
  return context.hasDocument() ? &context.document() : nullptr;
}

// manual_search, manual_section

ToolResult manualSearch(CallContext& context, const Args& args)
{
  auto request = pageRequest(args, 0);
  if (request.is_error())
  {
    return errorOf(request);
  }

  auto loaded = manualOf(context.host());
  if (loaded.is_error())
  {
    return errorOf(loaded);
  }
  const auto& manual = loaded.value()->manual;

  const auto resolver =
    ReferenceResolver{context.host(), documentOf(context), *loaded.value()};
  const auto texts = resolvedTexts(manual, resolver);

  auto items = std::vector<Json>{};
  for (const auto& hit : search(manual, texts, args.get<std::string>("query")))
  {
    const auto& section = manual.sections[hit.index];
    items.push_back(Json{
      {"id", section.id},
      {"title", section.title},
      {"level", section.level},
      {"path", manual.titlePath(hit.index)},
      {"score", hit.score},
      {"snippets", hit.snippets},
      {"uri", sectionUri(section)},
    });
  }
  return makePage(items, request.value(), 0);
}

/** The section's text, optionally followed by its nested sections with their headings. */
std::string sectionText(
  const Manual& manual,
  const std::vector<std::string>& texts,
  const size_t index,
  const bool includeSubsections)
{
  auto result = texts[index];
  if (includeSubsections)
  {
    const auto append = [&](const auto& self, const size_t parent) -> void {
      for (const auto child : manual.sections[parent].children)
      {
        const auto& section = manual.sections[child];
        result += fmt::format(
          "\n\n{} {}\n\n{}",
          std::string(std::min(section.level, size_t(6)), '#'),
          section.title,
          texts[child]);
        self(self, child);
      }
    };
    append(append, index);
  }
  return kdl::str_trim(result);
}

ToolResult manualSection(CallContext& context, const Args& args)
{
  auto loaded = manualOf(context.host());
  if (loaded.is_error())
  {
    return errorOf(loaded);
  }
  const auto& manual = loaded.value()->manual;

  const auto resolver =
    ReferenceResolver{context.host(), documentOf(context), *loaded.value()};
  const auto texts = resolvedTexts(manual, resolver);

  auto name = args.get<std::string>("section");
  if (name.starts_with(std::string{ManualUri} + "/"))
  {
    name =
      percentDecode(name.substr(std::string_view{ManualUri}.size() + 1)).value_or(name);
  }
  const auto index = manual.find(name);
  if (!index)
  {
    return unknownSectionError(manual, texts, name);
  }
  const auto& section = manual.sections[*index];

  const auto text =
    sectionText(manual, texts, *index, args.getOr("includeSubsections", false));
  const auto maxChars = size_t(args.get<int>("maxChars"));
  const auto offset =
    alignStart(text, std::min(size_t(args.getOr("offset", 0)), text.size()));

  auto end = std::min(text.size(), offset + maxChars);
  if (end < text.size())
  {
    // end at a line break in the second half of the page, else at a character boundary
    if (const auto lineBreak = text.rfind('\n', end);
        lineBreak != std::string::npos && lineBreak > offset + maxChars / 2)
    {
      end = lineBreak + 1;
    }
    else
    {
      end = alignEnd(text, end);
    }
  }

  auto subsections = Json::array();
  for (const auto child : section.children)
  {
    subsections.push_back(Json{
      {"id", manual.sections[child].id},
      {"title", manual.sections[child].title},
      {"level", manual.sections[child].level},
    });
  }

  return Json{
    {"id", section.id},
    {"title", section.title},
    {"level", section.level},
    {"path", manual.titlePath(*index)},
    {"uri", sectionUri(section)},
    {"text", text.substr(offset, end - offset)},
    {"offset", offset},
    {"length", end - offset},
    {"totalLength", text.size()},
    {"nextOffset", end < text.size() ? Json(end) : Json(nullptr)},
    {"subsections", std::move(subsections)},
    {"parent", sectionRef(manual, section.parent)},
    {"previous",
     sectionRef(manual, *index > 0 ? std::optional{*index - 1} : std::nullopt)},
    {"next", sectionRef(manual, *index + 1)},
  };
}

// Resources

ui::MapDocument* defaultDocument(ServerState& state, Session& session)
{
  const auto document = state.defaultDocument(session);
  return document ? document->document : nullptr;
}

Result<Json, ToolError> readTableOfContents(
  ServerState& state, Session&, const std::string& uri, const ResourceVariables&)
{
  auto loaded = manualOf(state.host);
  if (loaded.is_error())
  {
    return errorOf(loaded);
  }
  const auto& manual = loaded.value()->manual;

  auto sections = Json::array();
  for (const auto& section : manual.sections)
  {
    sections.push_back(Json{
      {"id", section.id},
      {"title", section.title},
      {"level", section.level},
      {"parent",
       section.parent ? Json(manual.sections[*section.parent].id) : Json(nullptr)},
      {"uri", sectionUri(section)},
    });
  }
  return jsonResourceContents(
    uri,
    Json{
      {"title", manual.title},
      {"sectionCount", manual.sections.size()},
      {"sections", std::move(sections)},
    });
}

Result<Json, ToolError> readSection(
  ServerState& state,
  Session& session,
  const std::string& uri,
  const ResourceVariables& variables)
{
  auto loaded = manualOf(state.host);
  if (loaded.is_error())
  {
    return errorOf(loaded);
  }
  const auto& manual = loaded.value()->manual;

  const auto resolver =
    ReferenceResolver{state.host, defaultDocument(state, session), *loaded.value()};
  const auto texts = resolvedTexts(manual, resolver);

  const auto name =
    percentDecode(variables.at("section")).value_or(variables.at("section"));
  const auto index = manual.find(name);
  if (!index)
  {
    return unknownSectionError(manual, texts, name);
  }

  const auto& section = manual.sections[*index];
  auto markdown = fmt::format(
    "{} {}\n\n{}",
    std::string(std::min(section.level, size_t(6)), '#'),
    section.title,
    texts[*index]);
  if (!section.children.empty())
  {
    markdown += "\n\nSubsections:\n";
    for (const auto child : section.children)
    {
      markdown += fmt::format(
        "\n- {} ({})", manual.sections[child].title, sectionUri(manual.sections[child]));
    }
  }

  return Json::array({Json{
    {"uri", uri},
    {"mimeType", "text/markdown"},
    {"text", markdown},
  }});
}

Schema sectionRefSchema()
{
  return any().describe("{id, title} or null");
}

} // namespace

void registerKnowledgeTools(ToolRegistry& registry)
{
  registry.add(
    ToolDef{"manual_search"}
      .title("Search Manual")
      .description(
        "Searches the TrenchBroom user manual (how the editor's tools, views, "
        "preferences and workflows work; read-only). Returns the matching sections, best "
        "first, "
        "with their parent titles and text snippets; read a section with "
        "manual_section. Menu items and shortcuts in the text are rendered as "
        "'**View > Maximize Current View** (Ctrl+Space)', with the current shortcuts. "
        "Examples: {\"query\": \"vertex tool\"}; {\"query\": \"smart tags\", "
        "\"limit\": 3}")
      .input(object({
        field("query", string().nonEmpty())
          .required()
          .describe(
            "Words to search for (case-insensitive); sections with more of them rank "
            "higher"),
        field("limit", integer().min(1).max(100).defaultsTo(10))
          .describe("Maximum number of sections to return"),
      }))
      .output(object({
        field(
          "items",
          array(object({
            field("id", string()).required(),
            field("title", string()),
            field("level", integer()),
            field("path", array(string())).describe("Titles of the enclosing sections"),
            field("score", number()).describe("Relevance, higher is better"),
            field("snippets", array(string())).describe("Text around the matches"),
            field("uri", string()).describe("The section's manual resource"),
          })))
          .required(),
        field("total", integer()).required(),
        field("nextCursor", any()),
        field("stale", boolean()),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Optional)
      .paginated()
      .idempotent()
      .handler(manualSearch));

  registry.add(
    ToolDef{"manual_section"}
      .title("Read Manual Section")
      .description(
        "Returns a section of the TrenchBroom user manual as Markdown, by id (from "
        "manual_search or the trenchbroom://manual table of contents) or exact title. "
        "Long texts are paged: pass 'offset' = nextOffset for the rest. With "
        "includeSubsections the nested sections follow with their headings. Also "
        "returns the subsections and the parent, previous and next sections. "
        "Read-only. Examples: {\"section\": \"camera_navigation\", "
        "\"includeSubsections\": true}; {\"section\": \"Vertex Editing\", \"offset\": "
        "12000}")
      .input(object({
        field("section", string().nonEmpty())
          .required()
          .describe("Section id (e.g. 'camera_navigation') or title"),
        field("offset", integer().min(0).defaultsTo(0))
          .describe(
            "Start of the returned text, in bytes (nextOffset of a previous call)"),
        field("maxChars", integer().min(500).max(100000).defaultsTo(12000))
          .describe("Maximum length of the returned text in bytes"),
        field("includeSubsections", boolean().defaultsTo(false))
          .describe("Append the nested sections with their headings"),
      }))
      .output(object({
        field("id", string()).required(),
        field("title", string()).required(),
        field("level", integer()),
        field("path", array(string())),
        field("uri", string()),
        field("text", string()).required(),
        field("offset", integer()).describe("Start of text in the section, in bytes"),
        field("length", integer()).describe("Length of text in bytes"),
        field("totalLength", integer()).describe("Length of the whole section in bytes"),
        field("nextOffset", any()).describe("Offset of the rest, or null"),
        field("subsections", array(any()))
          .describe("{id, title, level} per direct subsection"),
        field("parent", sectionRefSchema()),
        field("previous", sectionRefSchema()),
        field("next", sectionRefSchema()),
      }))
      .mutation(Mutation::None)
      .documentUse(DocumentUse::Optional)
      .idempotent()
      .handler(manualSection));
}

void registerKnowledgeResources(ResourceRegistry& registry)
{
  registry.add(ResourceDef{
    ManualUri,
    "manual",
    "User Manual",
    "Table of contents of the TrenchBroom user manual: each section's id, title, level, "
    "parent and uri (trenchbroom://manual/{section}). Static.",
    "application/json",
    readTableOfContents,
  });

  registry.addTemplate(ResourceTemplateDef{
    ManualSectionUriTemplate,
    "manual-section",
    "User Manual Section",
    "A section of the TrenchBroom user manual as Markdown ({section} is a section id "
    "from trenchbroom://manual, e.g. camera_navigation), with links to its subsections. "
    "Static.",
    "text/markdown",
    readSection,
  });
}

} // namespace tb::mcp
