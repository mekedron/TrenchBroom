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

#include "mcp/tools/Manual.h"

#include "kd/string_compare.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace tb::mcp
{
namespace
{

// A reference placeholder is RefStart, a kind character, the key and RefEnd.
constexpr auto RefStart = '\x01';
constexpr auto RefEnd = '\x02';

char referenceChar(const ManualReference kind)
{
  switch (kind)
  {
  case ManualReference::MenuItem:
    return 'm';
  case ManualReference::Action:
    return 'a';
  case ManualReference::Key:
    return 'k';
  }
  return 'k';
}

std::optional<ManualReference> referenceKind(const char c)
{
  switch (c)
  {
  case 'm':
    return ManualReference::MenuItem;
  case 'a':
    return ManualReference::Action;
  case 'k':
    return ManualReference::Key;
  default:
    return std::nullopt;
  }
}

bool isSpace(const char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

std::string lower(std::string_view str)
{
  return kdl::str_to_lower(str);
}

// HTML entities

void appendUtf8(std::string& out, const uint32_t cp)
{
  if (cp < 0x80)
  {
    out += char(cp);
  }
  else if (cp < 0x800)
  {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  }
  else if (cp < 0x10000)
  {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
  else if (cp < 0x110000)
  {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

std::optional<uint32_t> namedEntity(const std::string_view name)
{
  static const auto entities = std::unordered_map<std::string_view, uint32_t>{
    {"amp", '&'},       {"lt", '<'},       {"gt", '>'},       {"quot", '"'},
    {"apos", '\''},     {"nbsp", 0xA0},    {"laquo", 0xAB},   {"raquo", 0xBB},
    {"hellip", 0x2026}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"lsquo", 0x2018},
    {"rsquo", 0x2019},  {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"copy", 0xA9},
    {"reg", 0xAE},      {"times", 0xD7},   {"deg", 0xB0},     {"middot", 0xB7},
    {"larr", 0x2190},   {"rarr", 0x2192},  {"uarr", 0x2191},  {"darr", 0x2193},
    {"bull", 0x2022},   {"trade", 0x2122}, {"shy", 0xAD},     {"minus", 0x2212},
  };
  if (const auto it = entities.find(name); it != entities.end())
  {
    return it->second;
  }
  return std::nullopt;
}

std::string decodeEntities(const std::string_view text)
{
  auto result = std::string{};
  result.reserve(text.size());

  for (size_t i = 0; i < text.size(); ++i)
  {
    if (text[i] != '&')
    {
      result += text[i];
      continue;
    }

    const auto end = text.find(';', i);
    if (end == std::string_view::npos || end - i > 12)
    {
      result += '&';
      continue;
    }

    const auto name = text.substr(i + 1, end - i - 1);
    auto cp = std::optional<uint32_t>{};
    if (name.size() > 1 && name[0] == '#')
    {
      const auto hex = name[1] == 'x' || name[1] == 'X';
      const auto digits = name.substr(hex ? 2 : 1);
      auto value = uint32_t{0};
      auto valid = !digits.empty();
      for (const auto c : digits)
      {
        const auto digit = std::isdigit(static_cast<unsigned char>(c))
                             ? c - '0'
                             : (hex && std::isxdigit(static_cast<unsigned char>(c))
                                  ? std::tolower(static_cast<unsigned char>(c)) - 'a' + 10
                                  : -1);
        if (digit < 0 || value > 0x10FFFF)
        {
          valid = false;
          break;
        }
        value = value * (hex ? 16 : 10) + uint32_t(digit);
      }
      if (valid)
      {
        cp = value;
      }
    }
    else
    {
      cp = namedEntity(name);
    }

    if (cp)
    {
      appendUtf8(result, *cp == 0xA0 ? uint32_t(' ') : *cp);
      i = end;
    }
    else
    {
      result += '&';
    }
  }
  return result;
}

std::string collapseWhitespace(const std::string_view text)
{
  auto result = std::string{};
  auto space = false;
  for (const auto c : text)
  {
    if (isSpace(c))
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
  return result;
}

// Tags

struct Tag
{
  std::string name;
  bool closing = false;
  bool selfClosing = false;
  std::vector<std::pair<std::string, std::string>> attributes;

  std::optional<std::string> attribute(const std::string_view attributeName) const
  {
    for (const auto& [key, value] : attributes)
    {
      if (key == attributeName)
      {
        return value;
      }
    }
    return std::nullopt;
  }
};

/**
 * Parses the tag that starts at html[pos] ('<'). Returns the tag and the position after
 * it, or nullopt if there is no tag.
 */
std::optional<std::pair<Tag, size_t>> parseTag(const std::string_view html, size_t pos)
{
  auto tag = Tag{};
  ++pos;
  if (pos < html.size() && html[pos] == '/')
  {
    tag.closing = true;
    ++pos;
  }

  const auto nameStart = pos;
  while (pos < html.size() && std::isalnum(static_cast<unsigned char>(html[pos])))
  {
    ++pos;
  }
  if (pos == nameStart)
  {
    return std::nullopt;
  }
  tag.name = lower(html.substr(nameStart, pos - nameStart));

  while (pos < html.size())
  {
    while (pos < html.size() && isSpace(html[pos]))
    {
      ++pos;
    }
    if (pos >= html.size())
    {
      break;
    }
    if (html[pos] == '>')
    {
      return std::pair{std::move(tag), pos + 1};
    }
    if (html[pos] == '/')
    {
      tag.selfClosing = true;
      ++pos;
      continue;
    }

    const auto keyStart = pos;
    while (pos < html.size() && !isSpace(html[pos]) && html[pos] != '='
           && html[pos] != '>' && html[pos] != '/')
    {
      ++pos;
    }
    auto key = lower(html.substr(keyStart, pos - keyStart));
    auto value = std::string{};
    while (pos < html.size() && isSpace(html[pos]))
    {
      ++pos;
    }
    if (pos < html.size() && html[pos] == '=')
    {
      ++pos;
      while (pos < html.size() && isSpace(html[pos]))
      {
        ++pos;
      }
      if (pos < html.size() && (html[pos] == '"' || html[pos] == '\''))
      {
        const auto quote = html[pos];
        const auto valueEnd = html.find(quote, pos + 1);
        if (valueEnd == std::string_view::npos)
        {
          return std::nullopt;
        }
        value = html.substr(pos + 1, valueEnd - pos - 1);
        pos = valueEnd + 1;
      }
      else
      {
        const auto valueStart = pos;
        while (pos < html.size() && !isSpace(html[pos]) && html[pos] != '>')
        {
          ++pos;
        }
        value = html.substr(valueStart, pos - valueStart);
      }
    }
    if (key.empty())
    {
      ++pos;
      continue;
    }
    tag.attributes.emplace_back(std::move(key), decodeEntities(value));
  }
  return std::nullopt;
}

bool isVoidElement(const std::string_view name)
{
  return name == "img" || name == "br" || name == "hr" || name == "col" || name == "meta"
         || name == "link" || name == "input" || name == "source" || name == "wbr"
         || name == "area" || name == "base";
}

std::optional<size_t> headingLevel(const std::string_view name)
{
  if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6')
  {
    return size_t(name[1] - '0');
  }
  return std::nullopt;
}

size_t findCaseInsensitive(
  const std::string_view haystack, const std::string_view needle, const size_t from)
{
  for (auto i = from; i + needle.size() <= haystack.size(); ++i)
  {
    if (kdl::ci::str_is_equal(haystack.substr(i, needle.size()), needle))
    {
      return i;
    }
  }
  return std::string_view::npos;
}

// Markdown output

enum class CaptureMode
{
  /** Inline Markdown on one line (table cells). */
  Inline,
  /** Text only (headings). */
  Plain,
  /** Verbatim text (code blocks). */
  Raw,
};

struct ListState
{
  bool ordered = false;
  size_t counter = 0;
  /** The width of the current item's marker. */
  size_t indent = 0;
};

class MarkdownWriter
{
private:
  struct State
  {
    std::string out;
    size_t pendingBreak = 0;
    bool pendingSpace = false;
    bool suppressSpace = false;
    bool atItemStart = false;
    std::vector<ListState> lists;
    size_t quoteDepth = 0;
    std::optional<CaptureMode> mode;
  };

  State m_state;
  std::vector<State> m_saved;

public:
  bool markup() const { return !m_state.mode || *m_state.mode == CaptureMode::Inline; }

  bool raw() const { return m_state.mode == CaptureMode::Raw; }

  bool inlineOnly() const { return m_state.mode.has_value(); }

  void block(const size_t breaks)
  {
    if (inlineOnly())
    {
      space();
      return;
    }
    if (m_state.atItemStart)
    {
      return;
    }
    m_state.pendingBreak = std::max(m_state.pendingBreak, breaks);
    m_state.pendingSpace = false;
  }

  void lineBreak()
  {
    if (raw())
    {
      m_state.out += '\n';
    }
    else if (inlineOnly())
    {
      space();
    }
    else
    {
      m_state.pendingBreak = std::max(m_state.pendingBreak, size_t(1));
      m_state.atItemStart = false;
    }
  }

  void space()
  {
    if (!m_state.suppressSpace)
    {
      m_state.pendingSpace = true;
    }
  }

  void text(const std::string_view text)
  {
    if (raw())
    {
      m_state.out += text;
      return;
    }
    for (const auto c : text)
    {
      if (isSpace(c))
      {
        space();
      }
      else
      {
        put(std::string_view{&c, 1});
      }
    }
  }

  /** Writes inline content that is not split or collapsed. */
  void put(const std::string_view content)
  {
    if (raw())
    {
      m_state.out += content;
      return;
    }
    if (m_state.pendingBreak > 0)
    {
      flushBreak();
    }
    else if (m_state.pendingSpace && !m_state.out.empty() && m_state.out.back() != '\n')
    {
      m_state.out += ' ';
    }
    m_state.pendingSpace = false;
    m_state.suppressSpace = false;
    m_state.atItemStart = false;
    m_state.out += content;
  }

  void openMarker(const std::string_view marker)
  {
    if (markup())
    {
      put(marker);
      m_state.suppressSpace = true;
    }
  }

  void closeMarker(const std::string_view marker)
  {
    if (markup())
    {
      const auto pendingSpace = m_state.pendingSpace;
      m_state.pendingSpace = false;
      m_state.suppressSpace = false;
      m_state.out += marker;
      m_state.pendingSpace = pendingSpace;
    }
  }

  void openList(const bool ordered)
  {
    block(m_state.lists.empty() ? 2 : 1);
    m_state.atItemStart = false;
    m_state.lists.push_back(ListState{ordered, 0, 0});
  }

  void closeList()
  {
    if (!m_state.lists.empty())
    {
      m_state.lists.pop_back();
    }
    m_state.atItemStart = false;
    block(m_state.lists.empty() ? 2 : 1);
  }

  void openItem()
  {
    if (inlineOnly())
    {
      space();
      return;
    }
    if (m_state.lists.empty())
    {
      m_state.lists.push_back(ListState{});
    }

    auto& list = m_state.lists.back();
    list.indent = 0;
    m_state.atItemStart = false;
    m_state.pendingBreak = std::max(m_state.pendingBreak, size_t(1));
    flushBreak();

    const auto marker =
      list.ordered ? fmt::format("{}. ", ++list.counter) : std::string{"- "};
    m_state.out += marker;
    list.indent = marker.size();
    m_state.atItemStart = true;
    m_state.pendingSpace = false;
    m_state.suppressSpace = true;
  }

  void openQuote()
  {
    block(2);
    ++m_state.quoteDepth;
  }

  void closeQuote()
  {
    if (m_state.quoteDepth > 0)
    {
      --m_state.quoteDepth;
    }
    block(2);
  }

  /** Writes the given lines as a block, e.g. a table or a code block. */
  void blockLines(const std::vector<std::string>& lines)
  {
    if (lines.empty())
    {
      return;
    }
    if (inlineOnly())
    {
      for (const auto& line : lines)
      {
        text(line);
        space();
      }
      return;
    }

    m_state.atItemStart = false;
    block(2);
    put(lines.front());
    for (size_t i = 1; i < lines.size(); ++i)
    {
      m_state.out += '\n';
      m_state.out += prefix();
      m_state.out += lines[i];
    }
    block(2);
  }

  void beginCapture(const CaptureMode mode)
  {
    m_saved.push_back(std::move(m_state));
    m_state = State{};
    m_state.mode = mode;
  }

  std::string endCapture()
  {
    auto result = std::move(m_state.out);
    m_state = std::move(m_saved.back());
    m_saved.pop_back();
    return result;
  }

  /** Returns the text written so far and starts over. */
  std::string take()
  {
    auto result = std::move(m_state.out);
    m_state = State{};
    return result;
  }

private:
  std::string prefix() const
  {
    auto result = std::string{};
    for (size_t i = 0; i < m_state.quoteDepth; ++i)
    {
      result += "> ";
    }
    for (const auto& list : m_state.lists)
    {
      result += std::string(list.indent, ' ');
    }
    return result;
  }

  void flushBreak()
  {
    const auto breaks = m_state.pendingBreak;
    m_state.pendingBreak = 0;
    m_state.pendingSpace = false;
    if (m_state.out.empty())
    {
      m_state.out += prefix();
      return;
    }

    m_state.out += '\n';
    for (size_t i = 1; i < breaks; ++i)
    {
      for (size_t j = 0; j < m_state.quoteDepth; ++j)
      {
        m_state.out += '>';
      }
      m_state.out += '\n';
    }
    m_state.out += prefix();
  }
};

/** Removes trailing spaces of lines, surplus blank lines and leading and trailing blank
 * lines. */
std::string tidy(const std::string_view text)
{
  auto lines = std::vector<std::string>{};
  auto start = size_t{0};
  while (start <= text.size())
  {
    const auto end = text.find('\n', start);
    auto line = std::string{text.substr(
      start, end == std::string_view::npos ? std::string_view::npos : end - start)};
    while (!line.empty() && isSpace(line.back()))
    {
      line.pop_back();
    }
    lines.push_back(std::move(line));
    if (end == std::string_view::npos)
    {
      break;
    }
    start = end + 1;
  }

  auto result = std::string{};
  auto blank = false;
  for (const auto& line : lines)
  {
    if (line.empty())
    {
      blank = !result.empty();
      continue;
    }
    if (!result.empty())
    {
      result += blank ? "\n\n" : "\n";
    }
    blank = false;
    result += line;
  }
  return result;
}

std::string escapeCell(const std::string_view cell)
{
  return kdl::str_replace_every(collapseWhitespace(cell), "|", "\\|");
}

std::vector<std::string> tableLines(
  const std::vector<std::vector<std::string>>& rows, const size_t headerRows)
{
  if (rows.empty())
  {
    return {};
  }

  auto columns = size_t{0};
  for (const auto& row : rows)
  {
    columns = std::max(columns, row.size());
  }
  if (columns == 0)
  {
    return {};
  }

  const auto line = [&](const std::vector<std::string>& row) {
    auto result = std::string{"|"};
    for (size_t i = 0; i < columns; ++i)
    {
      result += " ";
      result += i < row.size() ? escapeCell(row[i]) : "";
      result += " |";
    }
    return result;
  };

  auto result = std::vector<std::string>{};
  // Markdown tables need a header row; a table without one gets an empty header
  const auto hasHeader = headerRows > 0;
  result.push_back(line(hasHeader ? rows.front() : std::vector<std::string>{}));
  auto separator = std::string{"|"};
  for (size_t i = 0; i < columns; ++i)
  {
    separator += " --- |";
  }
  result.push_back(separator);
  for (size_t i = hasHeader ? 1 : 0; i < rows.size(); ++i)
  {
    result.push_back(line(rows[i]));
  }
  return result;
}

std::string slug(const std::string_view title)
{
  auto result = std::string{};
  for (const auto c : title)
  {
    if (std::isalnum(static_cast<unsigned char>(c)))
    {
      result += char(std::tolower(static_cast<unsigned char>(c)));
    }
    else if (!result.empty() && result.back() != '-')
    {
      result += '-';
    }
  }
  while (!result.empty() && result.back() == '-')
  {
    result.pop_back();
  }
  return result;
}

/** The part of the document that holds the manual's content. */
std::string_view contentOf(const std::string_view html)
{
  if (const auto idPos = html.find("id=\"content_body\"");
      idPos != std::string_view::npos)
  {
    const auto tagStart = html.rfind('<', idPos);
    const auto tagEnd = html.find('>', idPos);
    const auto end = html.rfind("</article>");
    if (
      tagStart != std::string_view::npos && tagEnd != std::string_view::npos
      && end != std::string_view::npos && end > tagEnd)
    {
      return html.substr(tagEnd + 1, end - tagEnd - 1);
    }
  }

  if (const auto bodyPos = findCaseInsensitive(html, "<body", 0);
      bodyPos != std::string_view::npos)
  {
    const auto tagEnd = html.find('>', bodyPos);
    const auto end = findCaseInsensitive(html, "</body>", bodyPos);
    if (tagEnd != std::string_view::npos)
    {
      return html.substr(
        tagEnd + 1, end == std::string_view::npos ? end : end - tagEnd - 1);
    }
  }
  return html;
}

std::string documentTitle(const std::string_view html)
{
  const auto start = findCaseInsensitive(html, "<title>", 0);
  if (start == std::string_view::npos)
  {
    return {};
  }
  const auto end = findCaseInsensitive(html, "</title>", start);
  if (end == std::string_view::npos)
  {
    return {};
  }
  return collapseWhitespace(decodeEntities(html.substr(start + 7, end - start - 7)));
}

/** The placeholder for a script of the manual, or nullopt for other scripts. */
std::optional<std::string> scriptReference(const std::string_view script)
{
  static const auto pattern =
    std::regex{R"(print_(menu_item|action|key)\s*\(\s*["']([^"']*)["']\s*\))"};

  auto match = std::match_results<std::string_view::const_iterator>{};
  if (!std::regex_search(script.begin(), script.end(), match, pattern))
  {
    return std::nullopt;
  }

  const auto function = match[1].str();
  const auto kind = function == "menu_item" ? ManualReference::MenuItem
                    : function == "action"  ? ManualReference::Action
                                            : ManualReference::Key;
  // pandoc wraps long lines, also inside the script, and its smart typography turns "..."
  // into an ellipsis; both break the lookup
  const auto key =
    collapseWhitespace(kdl::str_replace_every(match[2].str(), "\u2026", "..."));
  return fmt::format("{}{}{}{}", RefStart, referenceChar(kind), key, RefEnd);
}

class ManualParser
{
private:
  std::string_view m_html;
  Manual m_manual;
  MarkdownWriter m_writer;

  /** Elements whose content is skipped: the name and the nesting depth. */
  std::optional<std::pair<std::string, size_t>> m_skip;

  std::optional<size_t> m_headingLevel;
  std::optional<std::string> m_headingId;

  struct Table
  {
    std::vector<std::vector<std::string>> rows;
    size_t headerRows = 0;
    bool inHead = false;
    bool rowHasHeaderCell = false;
    bool inCell = false;
  };
  std::vector<Table> m_tables;

  /** The hrefs of the open links; nullopt for links rendered as text. */
  std::vector<std::optional<std::string>> m_links;

public:
  explicit ManualParser(const std::string_view html)
    : m_html{html}
  {
  }

  Manual parse()
  {
    m_manual.title = documentTitle(m_html);

    const auto content = contentOf(m_html);
    auto pos = size_t{0};
    while (pos < content.size())
    {
      const auto next = content.find('<', pos);
      if (next == std::string_view::npos)
      {
        handleText(content.substr(pos));
        break;
      }
      handleText(content.substr(pos, next - pos));
      pos = handleMarkup(content, next);
    }

    finishSection();
    return std::move(m_manual);
  }

private:
  void handleText(const std::string_view text)
  {
    if (!m_skip && !text.empty())
    {
      m_writer.text(decodeEntities(text));
    }
  }

  /** Handles the markup at pos ('<') and returns the position after it. */
  size_t handleMarkup(const std::string_view content, const size_t pos)
  {
    if (content.substr(pos, 4) == "<!--")
    {
      const auto end = content.find("-->", pos + 4);
      return end == std::string_view::npos ? content.size() : end + 3;
    }
    if (content.substr(pos, 2) == "<!" || content.substr(pos, 2) == "<?")
    {
      const auto end = content.find('>', pos);
      return end == std::string_view::npos ? content.size() : end + 1;
    }

    auto parsed = parseTag(content, pos);
    if (!parsed)
    {
      handleText("<");
      return pos + 1;
    }

    auto& [tag, end] = *parsed;
    if (!tag.closing && (tag.name == "script" || tag.name == "style"))
    {
      const auto closeTag = "</" + tag.name;
      const auto closePos = findCaseInsensitive(content, closeTag, end);
      const auto body = content.substr(
        end,
        closePos == std::string_view::npos ? std::string_view::npos : closePos - end);
      if (!m_skip && tag.name == "script")
      {
        if (const auto reference = scriptReference(body))
        {
          m_writer.put(*reference);
        }
      }
      if (closePos == std::string_view::npos)
      {
        return content.size();
      }
      const auto closeEnd = content.find('>', closePos);
      return closeEnd == std::string_view::npos ? content.size() : closeEnd + 1;
    }

    if (m_skip)
    {
      if (tag.name == m_skip->first && !isVoidElement(tag.name) && !tag.selfClosing)
      {
        if (tag.closing)
        {
          if (--m_skip->second == 0)
          {
            m_skip = std::nullopt;
          }
        }
        else
        {
          ++m_skip->second;
        }
      }
      return end;
    }

    if (tag.closing)
    {
      closeTag(tag);
    }
    else
    {
      openTag(tag);
    }
    return end;
  }

  bool skips(const Tag& tag) const
  {
    return tag.name == "nav" || tag.name == "head" || tag.name == "button"
           || tag.name == "noscript" || tag.name == "template" || tag.name == "title"
           || (tag.name == "figcaption" && tag.attribute("aria-hidden") == "true")
           || tag.name == "colgroup";
  }

  void openTag(const Tag& tag)
  {
    if (skips(tag))
    {
      if (!isVoidElement(tag.name) && !tag.selfClosing)
      {
        m_skip = std::pair{tag.name, size_t{1}};
      }
      return;
    }

    const auto& name = tag.name;
    if (const auto level = headingLevel(name))
    {
      finishSection();
      m_headingLevel = level;
      m_headingId = tag.attribute("id");
      m_writer.beginCapture(CaptureMode::Plain);
    }
    else if (
      name == "p" || name == "div" || name == "section" || name == "article"
      || name == "main" || name == "figure" || name == "dl" || name == "aside"
      || name == "header" || name == "footer")
    {
      m_writer.block(2);
    }
    else if (name == "figcaption" || name == "dd")
    {
      m_writer.block(1);
    }
    else if (name == "dt")
    {
      m_writer.block(2);
      m_writer.openMarker("**");
    }
    else if (name == "br")
    {
      m_writer.lineBreak();
    }
    else if (name == "hr")
    {
      m_writer.blockLines({"---"});
    }
    else if (name == "ul" || name == "ol")
    {
      m_writer.openList(name == "ol");
    }
    else if (name == "li")
    {
      m_writer.openItem();
    }
    else if (name == "blockquote")
    {
      m_writer.openQuote();
    }
    else if (name == "pre")
    {
      m_writer.block(2);
      m_writer.beginCapture(CaptureMode::Raw);
    }
    else if (name == "strong" || name == "b")
    {
      m_writer.openMarker("**");
    }
    else if (name == "em" || name == "i")
    {
      m_writer.openMarker("*");
    }
    else if (name == "code" || name == "kbd")
    {
      m_writer.openMarker("`");
    }
    else if (name == "a")
    {
      const auto href = tag.attribute("href");
      if (href && !href->empty() && m_writer.markup())
      {
        m_writer.openMarker("[");
        m_links.push_back(*href);
      }
      else
      {
        m_links.push_back(std::nullopt);
      }
    }
    else if (name == "img")
    {
      const auto alt = tag.attribute("alt").value_or("");
      const auto src = tag.attribute("src").value_or("");
      if (m_writer.markup())
      {
        m_writer.put(fmt::format("![{}]({})", collapseWhitespace(alt), src));
      }
      else
      {
        m_writer.text(alt);
      }
    }
    else if (name == "table")
    {
      m_writer.block(2);
      m_tables.push_back(Table{});
    }
    else if (name == "thead" && !m_tables.empty())
    {
      m_tables.back().inHead = true;
    }
    else if (name == "tr" && !m_tables.empty())
    {
      m_tables.back().rows.emplace_back();
      m_tables.back().rowHasHeaderCell = false;
    }
    else if ((name == "td" || name == "th") && !m_tables.empty())
    {
      auto& table = m_tables.back();
      if (table.rows.empty())
      {
        table.rows.emplace_back();
      }
      if (name == "th")
      {
        table.rowHasHeaderCell = true;
      }
      closeCell();
      table.inCell = true;
      m_writer.beginCapture(CaptureMode::Inline);
    }
  }

  void closeTag(const Tag& tag)
  {
    const auto& name = tag.name;
    if (const auto level = headingLevel(name))
    {
      if (m_headingLevel)
      {
        startSection(collapseWhitespace(m_writer.endCapture()));
      }
    }
    else if (
      name == "p" || name == "div" || name == "section" || name == "article"
      || name == "main" || name == "figure" || name == "dl" || name == "figcaption"
      || name == "dd" || name == "aside" || name == "header" || name == "footer")
    {
      m_writer.block(2);
    }
    else if (name == "dt")
    {
      m_writer.closeMarker("**");
      m_writer.block(1);
    }
    else if (name == "ul" || name == "ol")
    {
      m_writer.closeList();
    }
    else if (name == "blockquote")
    {
      m_writer.closeQuote();
    }
    else if (name == "pre")
    {
      if (m_writer.raw())
      {
        auto code = m_writer.endCapture();
        while (!code.empty() && (code.back() == '\n' || code.back() == '\r'))
        {
          code.pop_back();
        }
        auto lines = std::vector<std::string>{"```"};
        for (auto& line : kdl::str_split(code, "\n"))
        {
          lines.push_back(std::move(line));
        }
        lines.push_back("```");
        m_writer.blockLines(lines);
      }
    }
    else if (name == "strong" || name == "b")
    {
      m_writer.closeMarker("**");
    }
    else if (name == "em" || name == "i")
    {
      m_writer.closeMarker("*");
    }
    else if (name == "code" || name == "kbd")
    {
      m_writer.closeMarker("`");
    }
    else if (name == "a")
    {
      if (!m_links.empty())
      {
        if (const auto href = m_links.back())
        {
          m_writer.closeMarker(fmt::format("]({})", *href));
        }
        m_links.pop_back();
      }
    }
    else if ((name == "td" || name == "th") && !m_tables.empty())
    {
      closeCell();
    }
    else if (name == "thead" && !m_tables.empty())
    {
      m_tables.back().inHead = false;
    }
    else if (name == "tr" && !m_tables.empty())
    {
      closeCell();
      auto& table = m_tables.back();
      if (
        (table.inHead || table.rowHasHeaderCell)
        && table.headerRows == table.rows.size() - 1)
      {
        ++table.headerRows;
      }
    }
    else if (name == "table" && !m_tables.empty())
    {
      closeCell();
      const auto table = std::move(m_tables.back());
      m_tables.pop_back();
      m_writer.blockLines(tableLines(table.rows, table.headerRows));
    }
  }

  void closeCell()
  {
    if (!m_tables.empty() && m_tables.back().inCell)
    {
      auto& table = m_tables.back();
      table.inCell = false;
      table.rows.back().push_back(kdl::str_trim(m_writer.endCapture()));
    }
  }

  void startSection(std::string title)
  {
    const auto level = *m_headingLevel;
    auto id = m_headingId.value_or("");
    if (id.empty())
    {
      id = slug(title);
    }
    m_headingLevel = std::nullopt;
    m_headingId = std::nullopt;

    auto section = ManualSection{
      .id = std::move(id),
      .title = std::move(title),
      .level = level,
    };

    const auto index = m_manual.sections.size();
    for (auto i = index; i > 0; --i)
    {
      if (m_manual.sections[i - 1].level < level)
      {
        section.parent = i - 1;
        m_manual.sections[i - 1].children.push_back(index);
        break;
      }
    }
    m_manual.sections.push_back(std::move(section));
  }

  void finishSection()
  {
    // close open cells and tables so that their content is not lost
    while (!m_tables.empty())
    {
      closeCell();
      const auto table = std::move(m_tables.back());
      m_tables.pop_back();
      m_writer.blockLines(tableLines(table.rows, table.headerRows));
    }
    auto text = tidy(m_writer.take());
    if (!m_manual.sections.empty())
    {
      m_manual.sections.back().text = std::move(text);
    }
  }
};

// shortcuts.js

/** A JavaScript literal: a string, an object, an array or anything else. */
struct JsValue
{
  enum class Kind
  {
    String,
    Object,
    Array,
    Other,
  };

  Kind kind = Kind::Other;
  std::string string;
  std::vector<std::pair<std::string, JsValue>> members;
  std::vector<JsValue> elements;

  const JsValue* member(const std::string_view name) const
  {
    for (const auto& [key, value] : members)
    {
      if (key == name)
      {
        return &value;
      }
    }
    return nullptr;
  }
};

class JsParser
{
private:
  std::string_view m_js;
  size_t m_pos = 0;

public:
  explicit JsParser(const std::string_view js)
    : m_js{js}
  {
  }

  /** The values of the top level declarations `const name = value;`. */
  std::map<std::string, JsValue> declarations()
  {
    auto result = std::map<std::string, JsValue>{};
    while (skipSpace(), m_pos < m_js.size())
    {
      const auto word = identifier();
      if (word == "const" || word == "let" || word == "var")
      {
        skipSpace();
        const auto name = identifier();
        skipSpace();
        if (!name.empty() && peek() == '=')
        {
          ++m_pos;
          if (auto value = parseValue())
          {
            result[name] = std::move(*value);
            continue;
          }
        }
      }
      if (word.empty())
      {
        ++m_pos;
      }
    }
    return result;
  }

private:
  char peek() const { return m_pos < m_js.size() ? m_js[m_pos] : '\0'; }

  void skipSpace()
  {
    while (m_pos < m_js.size())
    {
      if (isSpace(m_js[m_pos]))
      {
        ++m_pos;
      }
      else if (m_js.substr(m_pos, 2) == "//")
      {
        const auto end = m_js.find('\n', m_pos);
        m_pos = end == std::string_view::npos ? m_js.size() : end + 1;
      }
      else if (m_js.substr(m_pos, 2) == "/*")
      {
        const auto end = m_js.find("*/", m_pos + 2);
        m_pos = end == std::string_view::npos ? m_js.size() : end + 2;
      }
      else
      {
        break;
      }
    }
  }

  std::string identifier()
  {
    const auto start = m_pos;
    while (
      m_pos < m_js.size()
      && (std::isalnum(static_cast<unsigned char>(m_js[m_pos])) || m_js[m_pos] == '_' || m_js[m_pos] == '$'))
    {
      ++m_pos;
    }
    return std::string{m_js.substr(start, m_pos - start)};
  }

  std::optional<std::string> stringLiteral()
  {
    const auto quote = peek();
    ++m_pos;
    auto result = std::string{};
    while (m_pos < m_js.size() && m_js[m_pos] != quote)
    {
      if (m_js[m_pos] == '\\' && m_pos + 1 < m_js.size())
      {
        ++m_pos;
        const auto c = m_js[m_pos];
        result += c == 'n' ? '\n' : c == 't' ? '\t' : c;
      }
      else
      {
        result += m_js[m_pos];
      }
      ++m_pos;
    }
    if (m_pos >= m_js.size())
    {
      return std::nullopt;
    }
    ++m_pos;
    return result;
  }

  std::optional<JsValue> parseValue()
  {
    skipSpace();
    const auto c = peek();
    if (c == '\'' || c == '"')
    {
      if (auto str = stringLiteral())
      {
        return JsValue{.kind = JsValue::Kind::String, .string = std::move(*str)};
      }
      return std::nullopt;
    }
    if (c == '{')
    {
      ++m_pos;
      auto result = JsValue{.kind = JsValue::Kind::Object};
      while (true)
      {
        skipSpace();
        if (peek() == '}')
        {
          ++m_pos;
          return result;
        }
        auto key = std::optional<std::string>{};
        if (peek() == '\'' || peek() == '"')
        {
          key = stringLiteral();
        }
        else
        {
          key = identifier();
        }
        skipSpace();
        if (!key || key->empty() || peek() != ':')
        {
          return std::nullopt;
        }
        ++m_pos;
        auto value = parseValue();
        if (!value)
        {
          return std::nullopt;
        }
        result.members.emplace_back(std::move(*key), std::move(*value));
        skipSpace();
        if (peek() == ',')
        {
          ++m_pos;
        }
        else if (peek() != '}')
        {
          return std::nullopt;
        }
      }
    }
    if (c == '[')
    {
      ++m_pos;
      auto result = JsValue{.kind = JsValue::Kind::Array};
      while (true)
      {
        skipSpace();
        if (peek() == ']')
        {
          ++m_pos;
          return result;
        }
        auto value = parseValue();
        if (!value)
        {
          return std::nullopt;
        }
        result.elements.push_back(std::move(*value));
        skipSpace();
        if (peek() == ',')
        {
          ++m_pos;
        }
        else if (peek() != ']')
        {
          return std::nullopt;
        }
      }
    }

    // numbers, identifiers and anything else up to the next delimiter
    const auto start = m_pos;
    while (m_pos < m_js.size() && m_js[m_pos] != ',' && m_js[m_pos] != '}'
           && m_js[m_pos] != ']' && m_js[m_pos] != ';' && !isSpace(m_js[m_pos]))
    {
      ++m_pos;
    }
    if (m_pos == start)
    {
      return std::nullopt;
    }
    return JsValue{
      .kind = JsValue::Kind::Other,
      .string = std::string{m_js.substr(start, m_pos - start)}};
  }
};

/** Converts [{key: 'N', modifiers: ['Ctrl']}] to ["Ctrl+N"]. */
std::vector<std::string> jsShortcuts(const JsValue& value)
{
  auto result = std::vector<std::string>{};
  for (const auto& shortcut : value.elements)
  {
    const auto* key = shortcut.member("key");
    if (!key || key->string.empty())
    {
      continue;
    }
    auto parts = std::vector<std::string>{};
    if (const auto* modifiers = shortcut.member("modifiers"))
    {
      for (const auto& modifier : modifiers->elements)
      {
        parts.push_back(modifier.string);
      }
    }
    parts.push_back(key->string);
    result.push_back(kdl::str_join(parts, "+"));
  }
  return result;
}

struct CacheEntry
{
  std::filesystem::path path;
  std::filesystem::file_time_type modified;
  uintmax_t size = 0;
  std::shared_ptr<const LoadedManual> manual;
};

Result<std::string> readFile(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path, std::ios::binary};
  if (!stream)
  {
    return Error{fmt::format("Cannot open {}", path.string())};
  }
  auto buffer = std::ostringstream{};
  buffer << stream.rdbuf();
  return buffer.str();
}

} // namespace

std::optional<size_t> Manual::find(std::string_view idOrTitle) const
{
  const auto trimmed = kdl::str_trim(idOrTitle);
  auto key = std::string_view{trimmed};
  if (key.starts_with('#'))
  {
    key.remove_prefix(1);
  }
  for (size_t i = 0; i < sections.size(); ++i)
  {
    if (kdl::ci::str_is_equal(sections[i].id, key))
    {
      return i;
    }
  }
  for (size_t i = 0; i < sections.size(); ++i)
  {
    if (kdl::ci::str_is_equal(sections[i].title, key))
    {
      return i;
    }
  }
  return std::nullopt;
}

std::vector<std::string> Manual::titlePath(const size_t index) const
{
  auto result = std::vector<std::string>{};
  auto parent = sections[index].parent;
  while (parent)
  {
    result.push_back(sections[*parent].title);
    parent = sections[*parent].parent;
  }
  std::ranges::reverse(result);
  return result;
}

Manual parseManual(const std::string_view html)
{
  return ManualParser{html}.parse();
}

std::string resolveManualText(
  const std::string_view text, const ManualReferenceResolver& resolver)
{
  auto result = std::string{};
  result.reserve(text.size());

  auto pos = size_t{0};
  while (pos < text.size())
  {
    const auto start = text.find(RefStart, pos);
    if (start == std::string_view::npos)
    {
      result += text.substr(pos);
      break;
    }
    result += text.substr(pos, start - pos);

    const auto end = text.find(RefEnd, start);
    if (end == std::string_view::npos || end < start + 2)
    {
      pos = start + 1;
      continue;
    }
    if (const auto kind = referenceKind(text[start + 1]))
    {
      result += resolver(*kind, std::string{text.substr(start + 2, end - start - 2)});
    }
    pos = end + 1;
  }
  return result;
}

ManualShortcuts parseManualShortcuts(const std::string_view js)
{
  auto result = ManualShortcuts{};
  const auto declarations = JsParser{js}.declarations();

  if (const auto it = declarations.find("keys"); it != declarations.end())
  {
    for (const auto& [key, value] : it->second.members)
    {
      result.keys[key] = value.string;
    }
  }
  if (const auto it = declarations.find("menu"); it != declarations.end())
  {
    for (const auto& [key, value] : it->second.members)
    {
      auto item = ManualShortcuts::MenuItem{};
      if (const auto* path = value.member("path"))
      {
        for (const auto& element : path->elements)
        {
          item.path.push_back(element.string);
        }
      }
      if (const auto* shortcut = value.member("shortcut"))
      {
        item.shortcuts = jsShortcuts(*shortcut);
      }
      result.menu[key] = std::move(item);
    }
  }
  if (const auto it = declarations.find("actions"); it != declarations.end())
  {
    for (const auto& [key, value] : it->second.members)
    {
      result.actions[key] = jsShortcuts(value);
    }
  }
  return result;
}

Result<std::shared_ptr<const LoadedManual>> loadManual(const std::filesystem::path& path)
{
  static auto cache = std::optional<CacheEntry>{};

  auto error = std::error_code{};
  const auto modified = std::filesystem::last_write_time(path, error);
  if (error)
  {
    return Error{fmt::format("Cannot read {}: {}", path.string(), error.message())};
  }
  const auto size = std::filesystem::file_size(path, error);
  if (error)
  {
    return Error{fmt::format("Cannot read {}: {}", path.string(), error.message())};
  }

  if (cache && cache->path == path && cache->modified == modified && cache->size == size)
  {
    return cache->manual;
  }

  return readFile(path) | kdl::transform([&](const auto& html) {
           auto loaded = std::make_shared<LoadedManual>();
           loaded->path = path;
           loaded->manual = parseManual(html);

           const auto shortcutsPath = path.parent_path() / "shortcuts.js";
           if (auto js = readFile(shortcutsPath); js.is_success())
           {
             loaded->shortcuts = parseManualShortcuts(js.value());
           }

           auto manual = std::shared_ptr<const LoadedManual>{std::move(loaded)};
           cache = CacheEntry{path, modified, size, manual};
           return manual;
         });
}

} // namespace tb::mcp
