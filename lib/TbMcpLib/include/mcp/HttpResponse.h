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

#include "mcp/HttpParser.h"

#include <string>
#include <string_view>

namespace tb::mcp
{

/**
 * A complete HTTP response with a `Content-Length` body.
 */
struct HttpResponse
{
  int status = 200;
  /** Additional headers. `Content-Length` is added by `serializeHttpResponse`. */
  HttpHeaders headers = {};
  std::string body = {};
};

/**
 * Returns the reason phrase for the given status code, e.g. "Not Found" for 404.
 */
std::string_view httpReasonPhrase(int status);

/**
 * Serializes the given response including the status line and a `Content-Length`
 * header.
 */
std::string serializeHttpResponse(const HttpResponse& response);

/**
 * Serializes the status line and headers of a Server-Sent Events response. Adds the
 * headers `Content-Type: text/event-stream`, `Cache-Control: no-cache` and
 * `Transfer-Encoding: chunked` to the given ones. The body must be written with
 * `encodeHttpChunk` and terminated with `lastHttpChunk`.
 */
std::string serializeSseResponseHead(int status, const HttpHeaders& headers = {});

/**
 * Encodes the given data as one chunk of a chunked transfer encoding. Returns an empty
 * string for empty data because an empty chunk would terminate the body.
 */
std::string encodeHttpChunk(std::string_view data);

/**
 * Returns the terminating chunk of a chunked transfer encoding.
 */
std::string_view lastHttpChunk();

/**
 * Encodes one Server-Sent Events frame. Every line of `data` becomes a `data:` field.
 * The `event` and `id` fields are omitted if the corresponding argument is empty.
 */
std::string encodeSseEvent(
  std::string_view data, std::string_view event = {}, std::string_view id = {});

/**
 * Returns an SSE comment frame that keeps idle connections open.
 */
std::string_view sseKeepAliveComment();

} // namespace tb::mcp
