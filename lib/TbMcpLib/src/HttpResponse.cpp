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

#include "mcp/HttpResponse.h"

#include <fmt/format.h>

namespace tb::mcp
{
namespace
{

void appendHeaders(std::string& result, const HttpHeaders& headers)
{
  for (const auto& [name, value] : headers)
  {
    result += name;
    result += ": ";
    result += value;
    result += "\r\n";
  }
}

std::string statusLine(const int status)
{
  return fmt::format("HTTP/1.1 {} {}\r\n", status, httpReasonPhrase(status));
}

} // namespace

std::string_view httpReasonPhrase(const int status)
{
  switch (status)
  {
  case 200:
    return "OK";
  case 202:
    return "Accepted";
  case 204:
    return "No Content";
  case 400:
    return "Bad Request";
  case 401:
    return "Unauthorized";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 406:
    return "Not Acceptable";
  case 408:
    return "Request Timeout";
  case 411:
    return "Length Required";
  case 413:
    return "Content Too Large";
  case 415:
    return "Unsupported Media Type";
  case 431:
    return "Request Header Fields Too Large";
  case 500:
    return "Internal Server Error";
  case 503:
    return "Service Unavailable";
  case 505:
    return "HTTP Version Not Supported";
  default:
    return "Unknown";
  }
}

std::string serializeHttpResponse(const HttpResponse& response)
{
  auto result = statusLine(response.status);
  appendHeaders(result, response.headers);
  result += fmt::format("Content-Length: {}\r\n\r\n", response.body.size());
  result += response.body;
  return result;
}

std::string serializeSseResponseHead(const int status, const HttpHeaders& headers)
{
  auto result = statusLine(status);
  result +=
    "Content-Type: text/event-stream\r\n"
    "Cache-Control: no-cache\r\n"
    "Transfer-Encoding: chunked\r\n";
  appendHeaders(result, headers);
  result += "\r\n";
  return result;
}

std::string encodeHttpChunk(const std::string_view data)
{
  if (data.empty())
  {
    return {};
  }
  return fmt::format("{:X}\r\n{}\r\n", data.size(), data);
}

std::string_view lastHttpChunk()
{
  return "0\r\n\r\n";
}

std::string encodeSseEvent(
  std::string_view data, const std::string_view event, const std::string_view id)
{
  auto result = std::string{};
  if (!event.empty())
  {
    result += fmt::format("event: {}\n", event);
  }
  if (!id.empty())
  {
    result += fmt::format("id: {}\n", id);
  }

  // Every line of the data becomes a data field; CRLF, CR and LF all end a line.
  while (true)
  {
    const auto lineEnd = data.find_first_of("\r\n");
    result += "data: ";
    result += data.substr(0, lineEnd);
    result += "\n";
    if (lineEnd == std::string_view::npos)
    {
      break;
    }

    const auto skip = data.substr(lineEnd).starts_with("\r\n") ? 2u : 1u;
    data.remove_prefix(lineEnd + skip);
  }

  result += "\n";
  return result;
}

std::string_view sseKeepAliveComment()
{
  return ": keepalive\n\n";
}

} // namespace tb::mcp
