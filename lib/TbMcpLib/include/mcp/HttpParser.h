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

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tb::mcp
{

/**
 * A list of HTTP header fields in the order in which they were received or should be
 * sent.
 */
using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

/**
 * Returns the value of the first header with the given name, compared
 * case-insensitively, or nullopt if there is no such header.
 */
std::optional<std::string_view> findHttpHeader(
  const HttpHeaders& headers, std::string_view name);

/**
 * Compares the given strings ASCII case-insensitively.
 */
bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs);

struct HttpRequest
{
  std::string method;
  /** The request target as sent, e.g. `/mcp?x=1`. */
  std::string target;
  /** The request target without its query string, e.g. `/mcp`. */
  std::string path;
  /** Either 0 (HTTP/1.0) or 1 (HTTP/1.1). */
  int minorVersion = 1;
  HttpHeaders headers;
  std::string body;

  /**
   * Returns the value of the header with the given name (case-insensitive).
   */
  std::optional<std::string_view> header(std::string_view name) const;

  /**
   * Returns whether the connection should stay open after the response, according to
   * the HTTP version and the `Connection` header.
   */
  bool keepAlive() const;
};

/**
 * A parse error. `status` is the HTTP status code to answer with before closing the
 * connection, e.g. 400, 411, 413, 431 or 505.
 */
struct HttpParseError
{
  int status;
  std::string message;
};

/**
 * An incremental HTTP/1.1 request parser.
 *
 * Bytes are fed in arbitrary chunks as they arrive from the network. The parser supports
 * pipelined requests: every complete request is queued and can be taken with
 * `nextRequest()`. Only `Content-Length` bodies are supported; a request that uses
 * `Transfer-Encoding` is rejected with status 411.
 *
 * After an error, the parser ignores all further input. Requests that were completed
 * before the error remain available.
 */
class HttpParser
{
public:
  static constexpr std::size_t DefaultMaxBodySize = 16 * 1024 * 1024;
  static constexpr std::size_t DefaultMaxHeaderSize = 64 * 1024;

private:
  std::size_t m_maxBodySize;
  std::size_t m_maxHeaderSize;

  std::string m_buffer;
  std::optional<HttpRequest> m_current;
  std::size_t m_contentLength = 0;
  std::deque<HttpRequest> m_requests;
  std::optional<HttpParseError> m_error;

public:
  explicit HttpParser(
    std::size_t maxBodySize = DefaultMaxBodySize,
    std::size_t maxHeaderSize = DefaultMaxHeaderSize);

  /**
   * Consumes the given bytes and parses as many requests as possible.
   */
  void feed(std::string_view bytes);

  /**
   * Returns whether a complete request is available.
   */
  bool hasRequest() const;

  /**
   * Removes and returns the next complete request, or returns nullopt if there is none.
   */
  std::optional<HttpRequest> nextRequest();

  /**
   * Returns the parse error, if any.
   */
  const std::optional<HttpParseError>& error() const;

  /**
   * Returns the number of bytes that were fed but do not belong to a completed request
   * yet.
   */
  std::size_t bufferedSize() const;

private:
  void parse();
  bool parseHead();
  bool parseBody();
  void fail(int status, std::string message);
};

} // namespace tb::mcp
