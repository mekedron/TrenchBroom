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

#include "mcp/HttpParser.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace tb::mcp
{
namespace
{

bool isTokenChar(const char c)
{
  static constexpr auto specials = std::string_view{"!#$%&'*+-.^_`|~"};
  return std::isalnum(static_cast<unsigned char>(c))
         || specials.find(c) != std::string_view::npos;
}

bool isToken(const std::string_view str)
{
  return !str.empty() && std::ranges::all_of(str, isTokenChar);
}

std::string_view trimWhitespace(std::string_view str)
{
  while (!str.empty() && (str.front() == ' ' || str.front() == '\t'))
  {
    str.remove_prefix(1);
  }
  while (!str.empty() && (str.back() == ' ' || str.back() == '\t'))
  {
    str.remove_suffix(1);
  }
  return str;
}

std::optional<std::size_t> parseContentLength(const std::string_view str)
{
  if (str.empty() || !std::ranges::all_of(str, [](const char c) {
        return c >= '0' && c <= '9';
      }))
  {
    return std::nullopt;
  }

  auto result = std::size_t{0};
  for (const auto c : str)
  {
    const auto digit = static_cast<std::size_t>(c - '0');
    if (result > (std::numeric_limits<std::size_t>::max() - digit) / 10)
    {
      return std::numeric_limits<std::size_t>::max();
    }
    result = result * 10 + digit;
  }
  return result;
}

std::string pathFromTarget(std::string_view target)
{
  // absolute-form, e.g. http://localhost:47100/mcp
  if (const auto schemeEnd = target.find("://");
      schemeEnd != std::string_view::npos && !target.starts_with('/'))
  {
    const auto pathStart = target.find('/', schemeEnd + 3);
    target = pathStart != std::string_view::npos ? target.substr(pathStart) : "/";
  }

  const auto end = target.find_first_of("?#");
  return std::string{target.substr(0, end)};
}

/**
 * Checks whether the given comma separated header value contains the given token.
 */
bool containsToken(std::string_view value, const std::string_view token)
{
  while (!value.empty())
  {
    const auto comma = value.find(',');
    const auto element = trimWhitespace(value.substr(0, comma));
    if (equalsIgnoreCase(element, token))
    {
      return true;
    }
    if (comma == std::string_view::npos)
    {
      break;
    }
    value.remove_prefix(comma + 1);
  }
  return false;
}

} // namespace

bool equalsIgnoreCase(const std::string_view lhs, const std::string_view rhs)
{
  return lhs.size() == rhs.size()
         && std::ranges::equal(lhs, rhs, [](const char l, const char r) {
              return std::tolower(static_cast<unsigned char>(l))
                     == std::tolower(static_cast<unsigned char>(r));
            });
}

std::optional<std::string_view> findHttpHeader(
  const HttpHeaders& headers, const std::string_view name)
{
  const auto it = std::ranges::find_if(
    headers, [&](const auto& header) { return equalsIgnoreCase(header.first, name); });
  return it != headers.end() ? std::optional<std::string_view>{it->second} : std::nullopt;
}

std::optional<std::string_view> HttpRequest::header(const std::string_view name) const
{
  return findHttpHeader(headers, name);
}

bool HttpRequest::keepAlive() const
{
  const auto connection = header("Connection");
  if (minorVersion == 0)
  {
    return connection && containsToken(*connection, "keep-alive");
  }
  return !connection || !containsToken(*connection, "close");
}

HttpParser::HttpParser(const std::size_t maxBodySize, const std::size_t maxHeaderSize)
  : m_maxBodySize{maxBodySize}
  , m_maxHeaderSize{maxHeaderSize}
{
}

void HttpParser::feed(const std::string_view bytes)
{
  if (m_error)
  {
    return;
  }

  m_buffer.append(bytes);
  parse();
}

bool HttpParser::hasRequest() const
{
  return !m_requests.empty();
}

std::optional<HttpRequest> HttpParser::nextRequest()
{
  if (m_requests.empty())
  {
    return std::nullopt;
  }

  auto result = std::move(m_requests.front());
  m_requests.pop_front();
  return result;
}

const std::optional<HttpParseError>& HttpParser::error() const
{
  return m_error;
}

std::size_t HttpParser::bufferedSize() const
{
  return m_buffer.size();
}

void HttpParser::parse()
{
  while (!m_error)
  {
    if (!m_current && !parseHead())
    {
      return;
    }
    if (m_current && !parseBody())
    {
      return;
    }
  }
}

bool HttpParser::parseHead()
{
  // Ignore empty lines preceding the request line (RFC 9112, section 2.2)
  const auto firstNonEmpty = m_buffer.find_first_not_of("\r\n");
  m_buffer.erase(0, std::min(firstNonEmpty, m_buffer.size()));
  if (m_buffer.empty())
  {
    return false;
  }

  auto lines = std::vector<std::string_view>{};
  auto lineStart = std::size_t{0};
  while (true)
  {
    const auto lineEnd = m_buffer.find('\n', lineStart);
    if (lineEnd == std::string::npos)
    {
      if (m_buffer.size() > m_maxHeaderSize)
      {
        fail(431, "Request header fields too large");
      }
      return false;
    }

    auto line = std::string_view{m_buffer}.substr(lineStart, lineEnd - lineStart);
    if (line.ends_with('\r'))
    {
      line.remove_suffix(1);
    }

    lineStart = lineEnd + 1;
    if (lineStart > m_maxHeaderSize)
    {
      fail(431, "Request header fields too large");
      return false;
    }

    if (line.empty())
    {
      break;
    }
    lines.push_back(line);
  }

  // Request line: method SP request-target SP HTTP-version
  const auto requestLine = lines.front();
  const auto firstSpace = requestLine.find(' ');
  const auto lastSpace = requestLine.rfind(' ');
  if (firstSpace == std::string_view::npos || firstSpace == lastSpace)
  {
    fail(400, "Malformed request line");
    return false;
  }

  const auto method = requestLine.substr(0, firstSpace);
  const auto target = requestLine.substr(firstSpace + 1, lastSpace - firstSpace - 1);
  const auto version = requestLine.substr(lastSpace + 1);
  if (!isToken(method) || target.empty() || target.find(' ') != std::string_view::npos)
  {
    fail(400, "Malformed request line");
    return false;
  }

  if (!version.starts_with("HTTP/") || version.size() != 8 || version[6] != '.')
  {
    fail(400, "Malformed HTTP version");
    return false;
  }
  if (version[5] != '1' || (version[7] != '0' && version[7] != '1'))
  {
    fail(505, "HTTP version not supported");
    return false;
  }

  auto request = HttpRequest{
    std::string{method},
    std::string{target},
    pathFromTarget(target),
    version[7] - '0',
    {},
    {},
  };

  for (auto i = std::size_t{1}; i < lines.size(); ++i)
  {
    const auto line = lines[i];
    if (line.front() == ' ' || line.front() == '\t')
    {
      fail(400, "Obsolete header line folding is not supported");
      return false;
    }

    const auto colon = line.find(':');
    if (colon == std::string_view::npos || !isToken(line.substr(0, colon)))
    {
      fail(400, "Malformed header field");
      return false;
    }

    request.headers.emplace_back(
      std::string{line.substr(0, colon)},
      std::string{trimWhitespace(line.substr(colon + 1))});
  }

  if (request.header("Transfer-Encoding"))
  {
    fail(411, "Chunked request bodies are not supported, use Content-Length");
    return false;
  }

  auto contentLength = std::optional<std::size_t>{};
  for (const auto& [name, value] : request.headers)
  {
    if (equalsIgnoreCase(name, "Content-Length"))
    {
      const auto length = parseContentLength(value);
      if (!length || (contentLength && *contentLength != *length))
      {
        fail(400, "Invalid Content-Length");
        return false;
      }
      contentLength = length;
    }
  }

  if (contentLength && *contentLength > m_maxBodySize)
  {
    fail(413, "Request body too large");
    return false;
  }

  m_buffer.erase(0, lineStart);
  m_current = std::move(request);
  m_contentLength = contentLength.value_or(0);
  return true;
}

bool HttpParser::parseBody()
{
  if (m_buffer.size() < m_contentLength)
  {
    return false;
  }

  m_current->body = m_buffer.substr(0, m_contentLength);
  m_buffer.erase(0, m_contentLength);
  m_requests.push_back(std::move(*m_current));
  m_current = std::nullopt;
  m_contentLength = 0;
  return true;
}

void HttpParser::fail(const int status, std::string message)
{
  m_error = HttpParseError{status, std::move(message)};
  m_buffer.clear();
  m_current = std::nullopt;
}

} // namespace tb::mcp
