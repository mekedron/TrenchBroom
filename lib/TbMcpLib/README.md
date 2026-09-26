# TrenchBroom MCP Library

Contains the Qt-free core of TrenchBroom's [Model Context Protocol](https://modelcontextprotocol.io)
server, which lets AI agents inspect and edit maps. It links no `Qt6::` target, like
`TbAppLib`; the editor glue (TCP sockets, the Qt scheduler, preferences, the status bar
indicator) lives in `TbUiLib` (`McpServerController`, `McpTcpTransport`, `QtMcpHost`,
...), and the stdio bridge is the `TrenchBroomMcp` executable in `app/`.

The design is described in `docs/mcp/05-technical-design.md`.

This code is maintained in a fork of TrenchBroom. Keep MCP code in this library and in new
`Mcp*` files; change original TrenchBroom files only when there is no other way, and add
tests in new files of `TbMcpLibTest`, never in existing upstream test files (see the root
`CLAUDE.md`).

## Protocol

- `JsonRpc`, `ProtocolVersion`: JSON-RPC 2.0 messages and MCP revision negotiation
  (`2025-11-25`, `2025-06-18`, `2025-03-26`). JSON is `nlohmann::ordered_json` (`Json.h`).
- `McpServer`: the transport-neutral protocol engine. It implements `Endpoint`, the
  interface through which transports deliver POSTed payloads, open notification
  streams and delete sessions. It owns the sessions and the registries.
- `HttpParser`, `HttpResponse`, `SseParser`, `StreamableHttp`: the Streamable HTTP
  transport as a socket-free state machine over an abstract `HttpConnection`.

## Tools, resources and prompts

- `ToolRegistry` / `ToolDef`: declares a tool (name, title, description, input and output
  schema, mutation kind, handler). `Schema.h` is a small DSL that produces both the
  published JSON Schema and the argument validator.
- `ResourceRegistry`, `PromptRegistry`: static resources, URI templates, subscriptions,
  prompt templates.
- `tools/*`: the tool implementations, one file per domain. `RegisterAll` registers
  everything.

### Adding a tool

1. Pick the domain file in `src/tools/` (see the table in the design document) and add
   the handler: `ToolResult handler(CallContext& context, const Args& args)`.
2. Register it in that file's `register<Domain>Tools` with a `ToolDef`: title,
   description with an example, `input(...)`, `output(...)`, and the mutation kind:
   - `Mutation::None` for read-only tools (they run immediately),
   - `Mutation::Map` for map edits (the call runs in one transaction named
     `AI: <title>`, rolled back on error or dry run; `dryRun` and `document` are added
     to the input automatically),
   - `Mutation::External` for non-undoable side effects (the handler must honor
     `context.dryRun()`). Long operations use `.asyncHandler(...)` instead of `.handler(...)`:
     they report progress with `context.progress`, continue in steps scheduled with
     `context.defer`, check `context.cancelled()` between steps, and call the completion once
     (see `document_open`).
3. Change the map only through the `mdl::` free functions (`Map_*.h`). Use
   `resolveTargets` / `withTargets` (`Targets.h`) for tools that act on "ids or the
   current selection" (`faceTargetsField` / `resolveFaceTargets` / `withFaces` for tools
   that act on brush faces), `context.ids()` to format and resolve object ids, and return
   `makeError(ErrorCode::..., message, hint, objectIds)` on failure. If a `Map_*`
   function returns false, return `context.operationFailed(...)`, which includes what
   the editor logged. `context.addImage(bytes, "image/png")` adds an image to the result.
4. Add a `SECTION` to the domain's test case in `test/src/tst_<Domain>Tools.cpp` using
   `McpToolFixture`: success, invalid input, dry run, explicit ids vs. selection.

## Shared machinery

- `CallRunner`: the FIFO call queue, the "human is busy" gate, per-call transactions,
  dry runs, change reports, error mapping and the call log.
- `ObjectIds` (`IdRegistry`): stable object ids such as `brush:1042`, based on
  `mdl::Node::runtimeId()`, surviving undo/redo and linked group updates.
- `ChangeCollector`: created / modified / removed ids and introduced issues of a call.
- `Pagination`: cursors, limits and field selection for list tools.
- `CallLog`: the ring buffer behind `session_log` plus sinks (console, JSONL file).
- `Host`: the editor as seen by the server (`McpHost`, with the `DocumentHost` sub-interface
  that creates, loads and closes documents). The editor implements it in `TbUiLib`
  (`QtMcpHost`); tests use `FakeHost` from `TbMcpTestUtilsLib`.
- `LogCapture`: records the warnings and errors the editor logs during a call or while a
  document loads.
- `src/tools/ToolUtils`: small helpers shared by the tool files (game lookup, paths, times).
- `src/tools/GeometryUtils`: helpers of the geometry, brush editing and transform tools
  (brush builder with game defaults, material argument, world bounds and validity errors,
  `intersectsInterior`, `castRay`, brush classification, non-integer vertex warnings,
  `ScopedLockOverride`).
- `src/tools/EntityUtils`: helpers of the entity tools (definition lookup, property type
  descriptions, value validation with X14 warnings, flag lookup by name, `resolveEntities` /
  `withEntities` for tools that act on entities including worldspawn).
- `src/tools/NodeJson`: the shared JSON descriptions of objects and faces (`nodeSummary`,
  `nodeState`, `faceJson`); list and query tools use them so that all tools describe objects
  the same way.
- `ServerState::scheduleResourceUpdate` / `scheduleDocumentUpdate`: coalesced
  `resources/updated` notifications for subscribable resources.

## Tests

`TbMcpLibTest` runs headless over `ui::MapDocumentFixture` documents. `McpToolFixture`
(in `test-utils`) wires a server with all tools to a `FakeHost` and a `FakeScheduler`
and opens an initialized session:

```cpp
auto fixture = McpToolFixture{};
auto& document = fixture.create();
const auto result = fixture.call("history_get", Json{{"limit", 5}});
const auto error = fixture.callExpectingError("undo");
```
