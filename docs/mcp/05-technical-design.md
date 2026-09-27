# TrenchBroom MCP Server — Technical Design

Date: 2026-09-27 · Status: implemented for E1–E14; §13 lists the design of the remaining epics · Parent: [01-PRD.md](01-PRD.md) · Tools: [03-functional-spec.md](03-functional-spec.md) · Plan: [TASKS.md](TASKS.md)

This is the engineering blueprint of the MCP server. It describes the current design and
implementation. When the code and this document disagree, fix the code or update this document in the
same change.

---

## 0. Summary

| Topic | Design |
|---|---|
| Core library | Qt-free static library `lib/TbMcpLib`, namespace `tb::mcp`, headers in `include/mcp/`. It contains JSON-RPC, the MCP lifecycle, the HTTP/SSE protocol state machine, the registries, the call runner, the ID registry, and **all** tool implementations. |
| Editor glue | `lib/TbMcpUiLib` (links `TbUiLib`): `McpServerController`, `McpTcpTransport` (`QTcpServer`), `QtMcpHost` (implements the core's host interface), `McpCompileHost` (compiles with the editor's `CompilationRun`), `McpViewHost`, `McpActionHost`, `McpPreferenceHost` (the user's views, the action registry and the host-only preferences, E14), `QtScheduler`, `McpPreferencePane`, `McpStatusIndicator`, `McpUiIntegration`. |
| stdio | Executable `app/TrenchBroomMcp`: a stdio ↔ Streamable HTTP proxy (Qt Core + Network). |
| JSON | nlohmann/json 3.12.0 via CPM (`cmake/dependencies/nlohmann_json.cmake`). |
| Protocol | MCP revision `2025-11-25`; also accepts `2025-06-18` and `2025-03-26`. Streamable HTTP on `127.0.0.1:47100` (configurable), endpoint `/mcp`. |
| Threading | Single-threaded. All networking and all tool execution run on the Qt main thread's event loop. Modifying calls wait while the human is busy. |
| Object IDs | Process-unique `Node::runtimeId()` rendered as `brush:1042`. IDs survive undo/redo; linked-group re-cloning is handled with an alias table. |
| Atomicity | Each map-modifying call runs in a `Oneshot` transaction named `AI: <Tool title>`, cancelled on failure or dry run. Explicit agent transactions are `LongRunning` and nest the per-call ones. |
| Change report | Collected from `MapDocument` notifiers during the call, reduced to net created/modified/removed sets plus the selection and the issues the call introduced. |
| Errors | Tool failures are `CallToolResult{isError:true}` with a structured `error` object (`code`, `message`, `objectIds`, `hint`). JSON-RPC errors are used only for protocol faults. |
| Schemas | A C++ builder DSL produces both the JSON Schema that `tools/list` publishes and the validator/decoder of the call. One source of truth. |
| Vision and console | Offscreen snapshots from agent cameras (`McpSnapshotRenderer` behind the `SnapshotRenderer` seam) with per-snapshot visibility, never touching the user's views; a bounded buffer of all console messages, `console_read`, and a `console` report in every call result. |
| Tests | `TbMcpLibTest` (Catch2), headless over `MapDocumentFixture` with `FakeHost`, `FakeScheduler` and an in-process client (`McpToolFixture`). Qt transport, host and editor integration tests are in `TbMcpUiLibTest`. |

---

## 1. Library and target layout

### 1.1 Dependency graph

```
app/TrenchBroom ──► TbMcpUiLib ──► TbMcpLib ──► TbAppLib ──► TbMdlLib, TbRenderLib, TbPreferencesLib, ...
       │               │  │              └──────► nlohmann_json (PUBLIC)
       │               │  └──► Qt6::Network, Qt6::Widgets
       └──► TbUiLib ◄──┘
app/TrenchBroomMcp ──► TbMcpLib (BridgeSession, SseParser, JsonRpc) + TbVersionLib + Qt6::Core + Qt6::Network
```

`TbMcpLib` links no `Qt6::` target, like `TbAppLib`. It depends on `TbAppLib` because tools operate on
`ui::MapDocument`, which owns the `mdl::Map`, re-emits every `Map` notifier, and survives `reload()`
(which replaces the `Map`). Tools reach the map through `document.map()`; mutations go through the
`mdl::` free functions in `Map_*.h`.

### 1.2 `lib/TbMcpLib`

A `STATIC` library like `lib/TbAppLib`: `FILE_SET headers` with `BASE_DIRS include`, `PRIVATE CompilerConfig
PrecompileStdHeaders fmt::fmt-header-only miniz` (miniz writes the PNG images of `material_preview`), `PUBLIC nlohmann_json::nlohmann_json KdLib TbAppLib TbBaseLib
TbMdlLib VmLib`, plus `add_subdirectory(test-utils)` and `add_subdirectory(test)`.

```
lib/TbMcpLib/
  CMakeLists.txt
  README.md                 overview and the "adding a tool" checklist
  include/mcp/
    Json.h                  `using Json = nlohmann::ordered_json;` helpers, number rounding
    JsonVm.h                JSON conversion for vm::vec3d, vm::bbox3d, vm::plane3d, Color
    JsonRpc.h               message variant (Request/Notification/Response/Error), parse/serialize, error codes
    ProtocolVersion.h       supported revisions, negotiation
    Endpoint.h              transport-facing interface of McpServer (§3.2)
    McpServer.h             protocol engine: sessions, lifecycle, method dispatch (transport-neutral)
    ServerState.h           state shared with tools: host, scheduler, options, registries, sessions,
                            DocumentState per document, CallRunner, ServerActivity
    Session.h               per-client state: id, version, capabilities, subscriptions, active document, streams,
                            named agent cameras, kept snapshots, recent snapshot cameras (§10.12)
    Snapshot.h              SnapshotRequest/Scene/Options, UserView, the SnapshotRenderer interface (§10.12)
    AgentCamera.h           AgentCamera, camera math and framing helpers, camera JSON (§10.12)
    CameraProjection.h      makeGlCamera, ImageProjection: pixel rays and point projection (§10.12)
    Annotations.h           snapshot annotations drawn on the CPU: labels, grid, compass, player (§10.12)
    MapManifest.h           the per-map manifest <name>.mcp.json: model, JSON, ManifestStore (§10.13)
    Image.h                 RgbaImage, PNG encoding, side-by-side composition, changed-pixel diff, downscale
    ConsoleBuffer.h         bounded buffer of the editor's console messages (§9.1)
    HttpParser.h            incremental HTTP/1.1 request parser (Content-Length bodies only)
    HttpResponse.h          response and SSE frame serialization
    StreamableHttp.h        Streamable HTTP state machine over an abstract HttpConnection
    SseParser.h             incremental SSE parser (stdio bridge, tests)
    BridgeSession.h         the stdio bridge's offline answers, message routing and editor handshake (§3.5)
    Host.h                  McpHost, DocumentHost (§4.3)
    Scheduler.h             abstract Scheduler (post, postDelayed, now)
    Schema.h                schema builder DSL + validator/decoder (§7.2)
    Args.h                  typed access to validated arguments
    ToolRegistry.h          ToolDef, Mutation, DocumentUse, registry, tools/list paging
    ResourceRegistry.h      static resources, templates, subscriptions
    PromptRegistry.h        prompts/list, prompts/get
    CallContext.h           everything a handler sees (§7.2)
    CallRunner.h            call queue, busy gate, transactions, dry run, error mapping, logging
    ChangeCollector.h       notifier-based change report
    LogCapture.h            LogMessage, CapturingLogger, ScopedLogCapture, collectCachedMessages
    ObjectIds.h             IdRegistry, ObjectRef parsing/formatting, face refs
    Targets.h               idsField, resolveTargets, resolveFace, withTargets; face targets (§6.4)
    CompileRuns.h           compile run registry: run:<n> handles, jobs, log snapshots (§10.10)
    Errors.h                ToolError, ErrorCode, Warning, makeError
    Pagination.h            cursor/limit/fields/detail helpers, selectFields, base64
    CallLog.h               in-memory ring buffer, sinks, JsonlFileSink
    Resources.h             registerResources, agentGuide (§8)
    Prompts.h               registerPrompts (§8)
    RegisterAll.h           registerAll(McpServer&): every register<Domain>Tools + registerResources
    tools/<Domain>Tools.h   `void register<Domain>Tools(ToolRegistry&)` plus helpers shared with resources
                            (ConsoleTools.h also declares registerConsoleResources)
    tools/CompileUtils.h    compile presets per game family, tool path checks, profile JSON (§10.10)
    tools/CompileLog.h      compile log analysis: tasks, exit codes, errors, warnings, leaks (§10.10)
    tools/AssetUtils.h      the game file system and material images read on the CPU (§10.8)
    tools/MaterialKnowledge.h  face sampling, statistics, image analysis, notes, corpus, profiles (§10.8)
    tools/UvCheck.h         the texturing checks of uv_check and the material/UV tool warnings (§10.8)
    tools/EntityModelUtils.h   model loading, animations, frame property, placement checks (§10.7)
    tools/SpaceAnalysis.h   voxel grid of empty space, spaces and openings, free spots, walking, leaks (§10.13)
    tools/PlacementChecks.h z-fighting, per-call placement tracking and the MCP issue checks (§6.3, §10.13)
    tools/ActionCatalog.h   the MCP classification of every editor action: invoke, dialog or refuse, semantic tools (§10.9)
    tools/PreferenceCatalog.h  the editor's static preferences with categories and constraints, game preferences (§10.15)
    tools/Manual.h          the user manual parser: sections, Markdown text, shortcut references (§10.16)
  src/                      same names, .cpp; tools and private tool helpers in src/tools/ (§10)
  test/                     TbMcpLibTest (tst_<Unit>.cpp, fixture/)
  test-utils/               TbMcpTestUtilsLib: FakeHost, FakeScheduler, McpToolFixture
```

`lib/CMakeLists.txt` adds `TbMcpLib` before `TbMdlLib`. The test target copies `fixture/` to
`<bin>/fixture/test` and the `games` and `games-testing` resources to `<bin>/fixture/games`, like
`TbAppLibTest`. It links `Catch2::Catch2WithMain TbMcpLib TbMcpTestUtilsLib TbAppTestUtilsLib
TbBaseTestUtilsLib TbFsTestUtilsLib TbMdlTestUtilsLib` and calls `catch_discover_tests(TbMcpLibTest)`.

### 1.3 `lib/TbMcpUiLib` (namespace `tb::ui`)

A `STATIC` library with the editor glue, separate from `TbUiLib` so that `TbUiLib` and its CMake files stay
unchanged. Headers are in `include/ui/` (included as `ui/...`); it links `TbUiLib`, `TbMcpLib`,
`Qt6::Network` and `Qt6::Widgets`. Its tests are `TbMcpUiLibTest` (§11.2).

| File | Responsibility |
|---|---|
| `McpServerController.{h,cpp}` | The single start-up hook: created in `main()` after the `AppController` and destroyed before it, while the windows and documents still exist. Reads `--mcp-server` from `QCoreApplication::arguments()` (`setForceEnabled(true)`). Installs an application event filter that adds the status indicator to every `MapWindow` and the preference pane to every `PreferenceDialog` when they are shown (`McpUiIntegration`). Creates `mcp::McpServer` (with `registerAll`), `QtMcpHost`, `QtScheduler` and `McpTcpTransport` when enabled and destroys them when disabled. Watches all `MCP/*` preferences: bind address, port or token changes restart the server; the busy timeout is applied with `McpServer::setOptions`; `MCP/Log to file` toggles the JSONL sink. Writes and removes the discovery file (§3.4). `stopAgents()` → `McpServer::stopAgents` + close all connections. Signals `clientsChanged(int)`, `activityChanged(QString)`, `statusChanged()`. |
| `McpTcpTransport.{h,cpp}` | Adapts `StreamableHttpServer` to `QTcpServer`/`QTcpSocket` (§3.3). Byte I/O only. |
| `QtMcpHost.{h,cpp}` | Implements `mcp::McpHost` and `mcp::DocumentHost` over `AppController`, `MapWindowManager` and `MapWindow::toolBox()` (§4.3). Tracks the map windows with its own application event filter. |
| `QtScheduler.{h,cpp}` | `mcp::Scheduler` via `QTimer::singleShot` on the main thread. |
| `McpStatusIndicator.{h,cpp}` | Status bar widget after the update indicator: "AI: n clients · <current tool> / waiting for you / idle", and a **Stop agent** button. |
| `McpCompileHost.{h,cpp}` | Implements `mcp::CompileHost` with the editor's `CompilationRun` (§10.10). Owned by `QtMcpHost`. |
| `McpPreferencePane.{h,cpp}` | "AI Agents" pane added to `PreferenceDialog` (icon `McpPreferences.svg`): enable, port, bind address, access token (required only for non-loopback binding), log to file, busy-wait timeout. |

| `McpSnapshotRenderer.{h,cpp}` | Implements `mcp::SnapshotRenderer` with an offscreen GL context and framebuffer (§10.12). Owned by `QtMcpHost`, created on first use from `AppController::glManager()` and `findMapWindow` (only the user-view capture needs windows). |
| `McpViewHost.{h,cpp}` | Implements `mcp::ViewHost` over the `MapViewBase` widgets of a map window: cameras, maximized and current view (§10.14). `mapViewId` (the view ids "3d", "xy", "xz", "yz") is shared with `McpSnapshotRenderer`. |
| `McpActionHost.{h,cpp}` | Implements `mcp::ActionHost` over `AppController::actionManager()`: lists and runs the main menu, map view, tag and entity definition actions (§10.9). |
| `McpPreferenceHost.{h,cpp}` | Implements `mcp::PreferenceHost`: the MCP preferences and the shortcut preferences of all actions (§10.15). |
| `McpConsoleHook.{h,cpp}` | Owns the `mcp::ConsoleBuffer` and fills it from `Console::messageLoggedNotifier` (§9.1, §15). Created by `McpServerController` in its constructor, so it records from editor start whether or not the server runs, and outlives the server; `QtMcpHost::setConsoleHook` gives the host access. |
| `McpUiIntegration.{h,cpp}` | Adds the MCP widgets to editor windows and dialogs: `addMcpStatusIndicator` (appends the indicator to the window's status bar once); `addMcpPreferencePane` (adds the pane once with `PreferenceDialog::addPane`, §15). |
| `McpPreferences.h` | The MCP preferences, namespace `tb::McpPreferences` (below). |

```cpp
inline auto McpServerEnabled     = Preference<bool>{"MCP/Enabled", false};
inline auto McpServerPort        = Preference<int>{"MCP/Port", 47100};
inline auto McpServerBindAddress = Preference<std::string>{"MCP/Bind address", "127.0.0.1"};
inline auto McpServerAccessToken = Preference<std::string>{"MCP/Access token", ""};
inline auto McpLogToFile         = Preference<bool>{"MCP/Log to file", true};
inline auto McpBusyWaitTimeoutMs = Preference<int>{"MCP/Busy wait timeout", 30000};
```

`app/TrenchBroom/src/Main.cpp` registers the `--mcp-server` option, which enables the server for this process
regardless of the preference. The stdio bridge passes it when it launches the editor.

### 1.4 `app/TrenchBroomMcp`

Like `app/CmdTool`: `add_executable(TrenchBroomMcp)`, `EMBED_UTF8_MANIFEST`, links `CompilerConfig Qt6::Core
Qt6::Network TbMcpLib TbVersionLib`. It contains an offline `McpServer` with every tool, resource and prompt
(`BridgeSession`), so it links the tool code of `TbMcpLib`, but no Qt Widgets and no `TbUiLib`; its server
version is `VERSION_STR` from `TbVersionLib`, the same string as the editor's `getBuildVersion()`. Its CMake file makes `TrenchBroom` depend on it and installs it on Windows and Linux;
`app/TrenchBroom/CMakeLists.txt` copies it next to the editor after building (into
`TrenchBroom.app/Contents/MacOS/` on macOS) and passes it to `macdeployqt`. Behavior: §3.5.

### 1.5 Build note

An existing build tree must be configured with `-DFETCHCONTENT_UPDATES_DISCONNECTED=ON` before re-running
CMake; otherwise the git update step of the patched dependencies (assimp, cpptrace, miniz) re-runs and
re-applying their patches fails.

---

## 2. JSON library

nlohmann/json v3.12.0, fetched with CPM in `cmake/dependencies/nlohmann_json.cmake` (included from
`cmake/Dependencies.cmake`); header-only, so no `suppress_dependency_warnings` or `apply_sanitizer_options`:

```cmake
CPMAddPackage(
  URI "gh:nlohmann/json#v3.12.0"
  OPTIONS "JSON_BuildTests OFF" "JSON_Install OFF" "JSON_ImplicitConversions OFF"
)
```

Why: the core must be Qt-free (the `TbAppLib` rule and the headless mode), so `QJsonDocument` is out.
nlohmann is header-only, MIT, CMake-native, supports C++20, and has `ordered_json`: ordered keys make
responses deterministic and readable, and golden tests stable. Serialization cost is dominated by traversal.
`JSON_ImplicitConversions OFF` forces explicit `get<T>()` and avoids silent coercion in argument handling.

Conventions: `using Json = nlohmann::ordered_json`. Vectors are `[x, y, z]`, boxes `{"min":[..],"max":[..]}`.
Output doubles are rounded to 6 decimals (`mcp::roundForOutput`), so agents see `64`, not `63.99999999997`.

---

## 3. Transport

### 3.1 Protocol revisions and capabilities

`ProtocolVersion.h` lists `2025-11-25` (preferred), `2025-06-18` and `2025-03-26`. On `initialize` a
supported `protocolVersion` is echoed back; otherwise the server answers `2025-11-25`. JSON-RPC batch arrays
are accepted only on `2025-03-26` sessions (otherwise `-32600`). For `2025-03-26` sessions,
`structuredContent`, `outputSchema` and titles are omitted; the JSON is in the text block.

Capabilities: `tools{listChanged:true}`, `resources{subscribe:true, listChanged:true}`,
`prompts{listChanged:false}`, `logging{}`. `serverInfo = {name:"trenchbroom", title:"TrenchBroom",
version:<app version>}`. `instructions` points to the agent guide resource.

### 3.2 Endpoint and Streamable HTTP (core)

`Endpoint.h` is the transport-facing interface of `McpServer`:
`post(sessionId?, body, shared_ptr<RequestStream>) -> PostResult{Accepted|Pending|BadRequest|SessionNotFound,
newSessionId, body}`, `sessionProtocolVersion`, `openNotificationStream`, `deleteSession`. The server may
complete a `RequestStream` before `post` returns; transports buffer output until then so that the
`Mcp-Session-Id` header of a new session can be sent. The server holds only weak references to streams; a
dropped connection discards late output.

`StreamableHttpServer(Endpoint&, Config{bindAddress, accessToken, maxBodySize, path})` drives connections
through `HttpConnection{write, close}` with `openConnection`, `feed`, `connectionClosed`, `sendKeepAlives`
and `closeAllConnections`. It is fully unit-testable without sockets. One endpoint: `http://<bind>:<port>/mcp`.

- **POST** with a JSON-RPC body and `Accept: application/json, text/event-stream`.
  - Only notifications or responses: `202 Accepted`, no body.
  - Requests: `Content-Type: application/json` with the single response once it is ready. The response
    switches to `text/event-stream` when the call emits progress (it has a `progressToken`) or is
    asynchronous; then it streams the notifications and the final response and closes. The connection
    stays open while the call waits in the busy queue.
- **GET** with `Accept: text/event-stream`: the session's standalone SSE stream for server notifications
  (`resources/updated`, `tools/list_changed`, `notifications/message`). One per session. A `: keepalive`
  comment is sent every 15 s.
- **DELETE**: terminates the session (rolls back its agent transaction, drops subscriptions). A second GET or
  a DELETE ends the session's previous stream.
- **Sessions**: `initialize` creates a session and returns `Mcp-Session-Id` (128-bit random hex). Later
  requests must carry it: unknown id → `404`, missing → `400`. `MCP-Protocol-Version` must match the
  negotiated version; if absent the negotiated one is assumed.
- **SSE frames** carry `event: message` and `id: <seq>`; the POST and GET streams of a session share one
  sequence. `Last-Event-ID` replay is not supported (a reconnecting client re-reads resources).
- **Security**: `403` for an `Origin` that is present and not `http://localhost*` / `http://127.0.0.1*`.
  The `Host` header must match the bind address or `localhost` (DNS rebinding); a missing `Host` is `400`;
  a wildcard bind (`0.0.0.0`) skips the Host check. A non-loopback bind requires `Authorization: Bearer
  <McpServerAccessToken>` and refuses to start with an empty token. Loopback has no auth (PRD §7.1).
- **HTTP subset** (`HttpParser.h`): HTTP/1.1, `Content-Length` bodies (chunked request bodies → `411`),
  16 MiB body limit, 64 KiB header limit, keep-alive, incremental parsing. SSE responses use
  `Transfer-Encoding: chunked`. Wrong `Content-Type` → `415`; GET without an acceptable `Accept` → `406`.
  Transport-level errors carry JSON-RPC bodies.

QHttpServer is not used: it is a separate Qt module missing from some distro builds, and keeping the small
HTTP subset in the Qt-free core makes it testable and reusable in headless mode.

### 3.3 Qt adapter (`McpTcpTransport`)

`McpTcpTransport(Endpoint&, Config)` offers `listen(bind, port, token)`, `serverPort`, `errorString`,
`closeAllConnections` and the signal `connectionCountChanged`. `QTcpServer` runs on the main thread; each
`readyRead` feeds the socket's bytes to the core. If the port is taken, the error is logged, the status
indicator shows it, and the editor keeps working.

### 3.4 Discovery file

While listening, `McpServerController` atomically writes `<user data folder>/mcp-server.json`:
`{"port":47100,"bind":"127.0.0.1","pid":1234,"version":"2026.1"}`, and removes it on stop and quit. The
folder is `EnvironmentConfig::userDataFolderPath` (the same as `SystemPaths::userDataDirectory()` in
production, a temporary directory in tests). A killed editor leaves a stale file; the bridge tolerates it.

### 3.5 stdio bridge (`TrenchBroomMcp`)

- Reads newline-delimited JSON-RPC from stdin on a `std::thread` (`QSocketNotifier` cannot read stdin on
  Windows) and hands each line to the main thread with `QMetaObject::invokeMethod`. Lines that are not
  valid JSON are answered with `-32700` by the bridge itself.
- **Lazy start** (`mcp::BridgeSession`, Qt-free and unit-tested in `tst_BridgeSession.cpp`): starting a
  client does not start or contact the editor. The bridge answers `initialize`, `ping`, `tools/list`,
  `resources/list`, `resources/templates/list`, `prompts/list`, `prompts/get` and `logging/setLevel` itself
  with an offline `McpServer`: `registerAll` over a private host without editor, documents or games, a
  scheduler that runs nothing, and a `PreferenceManager` with default values
  (`BridgeSession::createNullPreferenceManager`). Its answers are the editor's for the same version
  (negotiation, capabilities, `serverInfo` with the version, `instructions`, lists); `resources/list` has only
  the static resources. `notifications/initialized` is consumed; other notifications and client responses are
  dropped while no editor session exists and nothing waits for one.
- **Editor session:** the first message that needs the editor (`tools/call`, `resources/read`,
  `resources/subscribe` / `unsubscribe`, `completion/complete`, unknown methods, a batch with any of them)
  is queued, and the bridge sends the handshake of `BridgeSession::handshake` one message at a time: the
  client's `initialize` parameters (request id `trenchbroom-bridge:initialize`, the response is swallowed;
  its `Mcp-Session-Id` and protocol version are kept), `notifications/initialized`, the client's log level,
  its resource subscriptions, then `tools/list` and `resources/list`. If the editor's lists differ from what
  the client knows (the bridge's own lists before the first connection), the bridge sends
  `notifications/tools/list_changed` / `notifications/resources/list_changed`. Then it opens the GET stream
  and forwards the queued messages in order. While the session is open, every message except `initialize`
  is forwarded as an HTTP POST (`QNetworkAccessManager`) with `Mcp-Session-Id` and `MCP-Protocol-Version`.
  JSON responses go to stdout as one line; SSE responses are parsed with `mcp::SseParser` and each `data:`
  event becomes one line. A new `initialize` from the client closes the editor session (DELETE).
- The GET stream reconnects after 1 s, 2 s, 5 s. When the editor refuses the stream's connection (it has
  quit) or answers `404`, the session is lost: the bridge sends `resources/list_changed` (and
  `tools/list_changed` if the editor's tools differed), answers the list requests itself again, and the next
  message that needs the editor opens a new session. A POST that gets `404` for its session (the editor was
  restarted) or a refused connection is queued again once and sent in the new session.
- It finds the editor through the discovery file in the directory of `SystemPaths::userDataDirectory()`
  (`~/.TrenchBroom` on Linux, the application data location elsewhere; portable mode is not handled).
- **Editor not running** (no discovery file, or connection refused) when a message needs it: it launches the
  sibling `TrenchBroom` executable with `--mcp-server` (`QProcess::startDetached`, stdin and stdout on the
  null device so that the editor never writes into the protocol stream) and polls every 250 ms for up to 30 s
  for a new or changed discovery file (so a stale file is ignored). Then the waiting requests get
  `-32000 "TrenchBroom did not start"`. With `--no-launch` they get `-32000` telling the user to start
  TrenchBroom with the MCP server enabled; the next message that needs the editor tries again.
- CLI: `TrenchBroomMcp [--port N] [--no-launch] [--editor PATH]`. Logs go to stderr only.
- Tools, resources and prompts need no bridge changes: the bridge has the same registries as the editor.

### 3.6 Multiple clients

Each `initialize` creates an independent `Session`. Notifications fan out to every session subscribed to
the resource (`resources/updated`) or to all sessions (`list_changed`). Tool calls from all sessions share
one FIFO call queue (§4.1), so edits never interleave. Each session has its own active document (default:
the focused window) and its own agent transaction.

---

## 4. Threading and dispatch

Everything runs on the Qt main thread: socket callbacks, parsing, dispatch, handlers and notifier
callbacks. No locks are needed around `Map`/`MapDocument`. Handlers must not block (budget < 100 ms for
simple edits); long work is asynchronous (§4.2).

### 4.1 Call queue and the "human busy" gate (spec X13)

Arguments are validated before a call is queued, so invalid calls fail immediately even while the human is
busy. Read-only tools (`Mutation::None`) bypass the queue and run immediately. `CallRunner` keeps one FIFO
of modifying calls:

1. The document is busy when `host.busyState(document)` returns `Busy` (`QtMcpHost`: a modal widget is
   open — `QApplication::activeModalWidget()` — or the window's `ToolBox::dragging()`), or when
   `map.commandProcessor().transactionDepth()` exceeds the depth the server itself opened (the human has a transaction open,
   e.g. a drag gesture).
2. While busy, the call stays queued and is re-checked every 50 ms; the status bar shows "AI waiting for
   you…". After `McpBusyWaitTimeoutMs` the call fails with `BUSY_TIMEOUT`.
3. When idle, `host.prepareForAgentEdit()` deactivates the current tool if it owns the selection
   (`selectionOwnedByTool()`), or is a node handle tool (vertex/edge/face) or the clip tool, as Escape would.
   The note (e.g. "deactivated tool: Vertex Tool") becomes a warning with code `EDITOR_STATE_CHANGED`.

`notifications/cancelled` removes a queued call; a running synchronous call cannot be interrupted and its
result is dropped. **Stop agent** clears the queue, abandons asynchronous calls, rolls back every agent
transaction, and closes all sessions (clients must re-initialize).

### 4.2 Asynchronous tools

`ToolDef::asyncHandler(fn)` with `fn(CallContext&, const Args&, ToolCompletion)` is for `Mutation::External`
and `Mutation::None` tools. External tools go through the queue, and the queue waits until the call
completes. Read-only (`None`) asynchronous tools — the snapshot tools (§10.12) — start immediately, skip the
busy wait, never open a transaction, and may run while other calls (also other asynchronous ones) run; they
have no `ScopedLogCapture` (it would interfere with the captures of the calls running meanwhile), so their
console messages are reported through the console buffer (§9.1). Their result is the handler's JSON, like
a synchronous read-only call. The handler continues in steps scheduled with `CallContext::defer(step,
delay = 0)` (a delay waits without blocking, e.g. for materials to load), checks `CallContext::cancelled()`
between them, and calls the completion once. `ctx.progress(progress, total, message)` emits
`notifications/progress` when the client sent a `progressToken`.

- `notifications/cancelled` for a running asynchronous call sets the cancelled flag (cooperative).
- Closing the session or **Stop agent** abandons the call with `CANCELLED` and drops its pending steps.
- If the target document closes meanwhile, the call fails with `DOCUMENT_NOT_FOUND`.
- An exception in a step becomes `INTERNAL_ERROR`.

`document_open`, `entity_definitions_reload` and `materials_reload` are asynchronous: they emit progress,
then load or reload in a deferred step, so a cancellation sent meanwhile is honored; the load itself is
synchronous. `compile_run` is not asynchronous in this sense: it starts the compilation and returns a
`run:<n>` handle immediately; the run is polled with `compile_status` (§10.10), so no request stays open
for minutes.

### 4.3 Host seam (`Host.h`)

```cpp
namespace tb::mcp {
enum class BusyState { Idle, Busy };
struct DocumentInfo { std::string id; ui::MapDocument* document; std::string windowTitle; bool focused; };
struct OpenedDocument { DocumentInfo document; std::vector<LogMessage> messages; };

class DocumentHost {
  virtual std::optional<DocumentInfo> documentToReplace() = 0;  // single-window mode, else nullopt
  virtual Result<OpenedDocument> createDocument(const mdl::GameInfo&, mdl::MapFormat) = 0;
  virtual Result<OpenedDocument> loadDocument(const mdl::GameInfo&, mdl::MapFormat /*Unknown = detect*/,
                                              const std::filesystem::path&) = 0;
  virtual void closeDocument(ui::MapDocument&) = 0;  // no questions; object stays alive until the event loop
  virtual std::vector<std::filesystem::path> recentDocuments() = 0;
};

class McpHost {  // ui::QtMcpHost, mcp::FakeHost (tests), later mcp::HeadlessHost (E16)
public:
  Notifier<ui::MapDocument&> documentWillCloseNotifier;    // before a document is destroyed
  Notifier<> documentsDidChangeNotifier;                   // open, close, focus change
  Notifier<ui::MapDocument&> currentToolDidChangeNotifier; // active tool of a document's window
  virtual std::string applicationVersion() const = 0;
  virtual std::vector<DocumentInfo> documents() = 0;       // window order
  virtual BusyState busyState(ui::MapDocument&) = 0;
  virtual std::vector<std::string> prepareForAgentEdit(ui::MapDocument&) = 0;  // returns notes
  virtual std::optional<std::string> currentToolName(ui::MapDocument&) = 0;
  virtual bool isCompileRunning(ui::MapDocument&) = 0;
  virtual DocumentHost& documentHost() = 0;
  virtual mdl::GameManager& gameManager() = 0;
  virtual CompileHost* compileHost();                       // default nullptr → UNSUPPORTED_IN_HOST
  virtual EngineHost* engineHost();                         // default nullptr → engine_launch UNSUPPORTED_IN_HOST
  virtual Logger* logTarget(ui::MapDocument&);               // the window console, default nullptr
  virtual SnapshotRenderer* snapshotRenderer();             // §10.12, default nullptr → UNSUPPORTED_IN_HOST
  virtual ConsoleBuffer* consoleBuffer();                   // §9.1, default nullptr → UNSUPPORTED_IN_HOST
  virtual void clearConsoleViews();                         // console_clear, default no-op
  virtual std::optional<std::filesystem::path> knowledgeDirectory(); // §10.8, default nullopt
  virtual ViewHost* viewHost();                             // §10.14, default nullptr → UNSUPPORTED_IN_HOST
  virtual ActionHost* actionHost();                         // §10.9, default nullptr → UNSUPPORTED_IN_HOST
  virtual PreferenceHost* preferenceHost();                 // §10.15, default nullptr (no host preferences)
  virtual std::optional<std::filesystem::path> manualPath(); // §10.16, default nullopt → UNSUPPORTED_IN_HOST
};

class ViewHost {    // ui::McpViewHost, FakeViewHost (tests)
  virtual std::vector<UserView> views(ui::MapDocument&) = 0;          // 3d, xy, xz, yz with cameras
  virtual Result<void> setCamera(ui::MapDocument&, const std::string& viewId, const AgentCamera&) = 0;
  virtual Result<ViewLayout> layout(ui::MapDocument&) = 0;            // maximized and current view
  virtual Result<void> setMaximizedView(ui::MapDocument&, const std::optional<std::string>& viewId) = 0;
  virtual void prepareForLayoutChange() = 0;                          // focus out of the views
};

class ActionHost {  // ui::McpActionHost, FakeActionHost (tests)
  // All actions for the document's window; enabled/checked evaluated for the view (default: current).
  virtual Result<std::vector<EditorAction>> actions(ui::MapDocument&, const std::optional<std::string>& viewId) = 0;
  // Runs the action now, or after control returned to the event loop (deferred, for dialogs).
  virtual Result<EditorAction> invokeAction(ui::MapDocument&, const std::string& path,
                                            const std::optional<std::string>& viewId, bool deferred) = 0;
};

class PreferenceHost {  // ui::McpPreferenceHost, FakePreferenceHost (tests)
  // AnyPreference = variant of Preference<T>* (bool, int, float, string, path, Color, shortcuts); the pointers
  // stay valid until the next call. HostPreference: preference, category, description, minimum, maximum,
  // lockedReason (agents cannot change it), secret (value reported as null).
  virtual std::vector<HostPreference> preferences(ui::MapDocument*) = 0;
};

class CompileJob {  // destroying a running job terminates it without callbacks
  virtual std::string log() const = 0;  // plain text as the compilation dialog shows it
  virtual bool running() const = 0;
  virtual void cancel() = 0;            // ends the job; `ended` may fire before it returns
};
struct CompileJobCallbacks { std::function<void()> outputChanged; std::function<void()> ended; };

class CompileHost {
  // Runs the profile's enabled tasks with the editor's compilation variables; test = only log.
  // Callbacks may fire before startCompile returns. A reload of the document cancels the job.
  virtual Result<std::unique_ptr<CompileJob>> startCompile(ui::MapDocument&, const mdl::CompilationProfile&,
                                                           bool test, CompileJobCallbacks) = 0;
};

class EngineHost {  // ui::McpEngineHost, FakeEngineHost (tests)
  // Interpolates a parameter spec with the editor's launch variables (LaunchGameEngineVariables).
  virtual Result<std::string> engineParameters(ui::MapDocument&, const std::string& parameterSpec) = 0;
  // Starts the profile's engine detached (parameters default to the profile's spec); returns the process id.
  virtual Result<int64_t> launchEngine(ui::MapDocument&, const mdl::GameEngineProfile&,
                                       std::optional<std::string> parameterSpec) = 0;
};
}
```

Game and format detection (`readMapHeader`) happens in the core. Game paths are set with `setPref` on the
game's `gamePathPreference` directly (Qt-free; open documents react through their preference observer).

`QtMcpHost`:
- Document handles `doc:<n>` follow window open order. An application event filter tracks the windows: the
  `Show` event of a new `MapWindow` and `QApplication::focusChanged` (window order) → `documentsDidChangeNotifier`;
  the `DeferredDelete` event of a `MapWindow` (closed windows use `WA_DeleteOnClose`; the document is still
  alive, but the manager no longer lists the window) → `documentWillCloseNotifier`, then
  `documentsDidChangeNotifier`; each document's `documentWasLoadedNotifier` (created or loaded in place) →
  `documentsDidChangeNotifier`; each window's `ToolBox` `toolActivatedNotifier`/`toolDeactivatedNotifier` →
  `currentToolDidChangeNotifier`. `logTarget` returns the window's console (`MapWindow::logger()`).
- Creates documents with `MapDocument::createDocument/loadDocument` and shows them with
  `MapWindowManager::createMapWindow`; in single-window mode the top window's document is recreated in
  place. `closeDocument` calls `MapWindow::closeDiscardingChanges` (§15). Agent-created documents do not
  close the welcome window. `isCompileRunning` asks the window's compilation dialog
  (`MapWindow::compilationDialog()`, `CompilationDialog::running()`, §15).

`QtMcpHost::knowledgeDirectory()` is `mcp-knowledge` in the user data folder; `FakeHost` uses
`mcp-knowledge` in its temporary directory (`knowledgeDir`, nullopt simulates a host without one).

`QtMcpHost::compileHost()` returns its `McpCompileHost`, whose camera provider copies the perspective camera
of the document's map window (`MapWindow::mapView()`, §15) (used by export tasks that add an entity at the camera position).

`QtMcpHost::engineHost()` returns its `McpEngineHost`, which calls the editor's `launchGameEngineProfile` with the
`LaunchGameEngineVariables` of the document's map, like the Launch Engine dialog, and reports the process id (§15).

`FakeHost` implements both interfaces with its own task and resource managers and a `GameManager` with the
games "Test", "Quake", "Quake 2", "Half-Life" and "Quake 3" (the real configurations from the fixture's
`games/` folder; game paths in `test/mdl/Game/` for Quake and Quake 2, empty folders in a temporary
directory for the others). The game manager writes compile and engine profiles to a temporary config
folder (`configDir()`), removed with the host. `singleWindow` simulates single-window mode,
`recentDocumentList` is the recent list, and closed documents stay alive. `compile` is a `FakeCompileHost`
whose `FakeCompileJob`s the test drives with `append` / `finish` (`onStart` can finish a job synchronously
like a test run, `startError` makes the start fail); `compileHostOverride` substitutes another compile host
(`TbMcpUiLibTest` uses the real `McpCompileHost`), and `supportsCompile = false` simulates a host without one.
`engine` is a `FakeEngineHost` that records launches (document, profile, parameter spec, interpolated parameters,
process id from `nextProcessId`) instead of starting processes; its `engineParameters` replaces only
`${MAP_BASE_NAME}`. `startError` and `parametersError` simulate failures, and `supportsEngine = false` simulates a
host without one.

`FakeHost` has `view` (`FakeViewHost`: four views, linked 2D cameras like `CameraLinkHelper`, `hasWindow`,
recorded calls), `action` (`FakeActionHost`: `actionList`, recorded invocations, simulated failures; `actionHostOverride`
substitutes the real host in `TbMcpUiLibTest`), `preference` (`FakePreferenceHost`) and `manualFile`; `supportsViews`,
`supportsActions` and `supportsPreferences` simulate hosts without them. A host that does not implement a capability
maps to `UNSUPPORTED_IN_HOST`.

`QtMcpHost` owns `McpViewHost`, `McpActionHost` and `McpPreferenceHost`; `manualPath()` is
`SystemPaths::findResourceFile("manual/index.html")` (the generated manual, in the build tree and in the installed
application).

### 4.4 Server state

`ServerState` holds everything tools may need; handlers reach it through `CallContext::server()` and tests
through `McpServer::state()`. It creates a `DocumentState` (`IdRegistry`, open `AgentTransaction`, resource
change hooks) for every open document whenever the document list changes, so subscriptions work before any
tool touched a document, and drops it on `documentWillCloseNotifier`. `DocumentState::disabledValidators` holds the validators turned off with `validators_set` (§10.13). It owns the `CompileRuns` registry
(§10.10) and the server clipboard (`ServerState::clipboard`, §10.11); `ServerState::isCompileRunning(document)` is true while an MCP run or the editor's compilation
dialog compiles the document.

---

## 5. Stable object IDs

### 5.1 Facts in the model

- **Node pointers survive undo/redo.** `AddRemoveNodesCommand` keeps removed `Node*`s and re-adds the same
  pointers on undo; `ReparentNodesCommand` moves the same pointers; `SwapNodeContentsCommand` swaps contents
  inside the same `Node*`. Selection, visibility and lock commands do not touch identity.
- **Removed nodes are freed later** without notification (when the redo stack is cleared), so addresses
  can be recycled. Raw pointers are not safe as IDs.
- **Linked groups re-clone children.** `UpdateLinkedGroupsHelper::doReplaceChildren` replaces all children
  of each target linked group with fresh clones (`nodesWereRemoved(old)`, then `nodesWereAdded(new)`); the
  clones keep the source's `Object::linkId()`.
- **Persistent ids are not general.** `persistentId()` exists only for layers and groups. `linkId()` is
  shared by corresponding objects in linked groups and copied by `clone()`.
- `Map::reload()` builds a new `Map`; `MapDocument::setMap` replaces it (then `documentWasLoadedNotifier`).

### 5.2 Design

1. `IdType Node::runtimeId() const` (TbMdlLib) is assigned in `Node::Node()` from a
   `static std::atomic<IdType>` counter starting at 1 (atomic because parsing may construct nodes on worker
   threads). It is never copied (the private copy constructor also draws a fresh id), never persisted, and
   never reused within the process.
2. **Format** `<kind>:<runtimeId>`, kind ∈ `world | layer | group | entity | brush | patch`, e.g.
   `brush:1042`. The world is always `world` and the default layer always `layer:default` (canonical, also
   accepted on input). Faces are `brush:1042/face:3`, the index into `BrushNode::brush().faces()`; a face id
   resolves to its brush, and `resolveFace` (`Targets.h`) returns the `BrushFaceHandle`. Other handles:
   `doc:<n>` (documents), `run:<n>` (compile runs, E7), `issue:<runtimeId>:<issueType>:<k>` (issues, E13).
3. **`IdRegistry`** (one per `MapDocument`) maps `runtimeId → Node*` for nodes currently in the tree.
   - Built by a full tree walk on attach and on `documentWasLoadedNotifier`; `nodesWereAdded` registers nodes
     and descendants, `nodesWereRemoved` unregisters them, so a pointer is never dereferenced after it could
     have been freed.
   - An unregistered id gives `OBJECT_NOT_FOUND`, saying the object may have been deleted and `undo` may
     restore it. Undo re-adds the same `Node*`, so the old id becomes valid again.
   - **Linked-group aliasing:** within one notifier burst (removed, then added under the same parent), an
     added node whose `(parent GroupNode*, path of linkIds)` matches a removed node gets
     `alias[newRuntimeId] = canonicalId(oldNode)`. `formatId` emits the canonical id and `resolve` follows
     aliases, so a brush in a linked copy keeps its id when another copy is edited. The change collector
     reports it as modified.
   - **Reload/revert:** the registry keeps the persistent id of every layer and group it has seen; old
     layer/group ids resolve to the node with the same persistent id. Other ids fail with "document was
     reloaded". `document_revert` returns `"idsInvalidated": true`.
4. **Faces:** a face index is valid until the brush's geometry changes. Brush payloads list faces with
   `index`, `normal`, `center` and `material`; the change report lists geometry-changed brushes under
   `modified`, and the guide says to re-read faces then.
5. `object_get` exposes `persistentId` (layers/groups) and `linkId` (linked-group reasoning).

---

## 6. Transactions, atomicity, dry run, change report

### 6.1 Per-call transaction (`CallRunner`)

For `Mutation::Map` tools with `transactional(true)` (the default):

```
collector.attach(document)                                   // §6.3
map.startTransaction("AI: " + def.title, TransactionScope::Oneshot)
status = handler(ctx, args)                                  // Map_* free functions
report = collector.finish()                                  // before any rollback, so dry runs get it
if (!status || ctx.dryRun()) map.cancelTransaction();        // rollback, no undo entry
else commit with command collation disabled                  // failure → OPERATION_FAILED
```

- `Map_*` functions open their own transactions, which nest and fold into ours: the undo menu shows exactly
  one `AI: <title>` entry (X2). Collation is disabled while committing because `CommandProcessor` would
  otherwise merge two consecutive agent calls whose first commands collate.
- `undoStep` is reported only if the transaction actually stored a command (observed through
  `transactionDoneNotifier`); calls that change nothing report `null`.
- Handlers fail by returning `ToolError`. When a `Map_*` call returns `false`, `ctx.operationFailed(...)`
  builds `OPERATION_FAILED` with the messages the document logged during the call. `ScopedLogCapture`
  (`LogCapture.h`) re-targets the document's `LoggingHub` to a capturing logger that forwards to the original
  target, which the host provides (`McpHost::logTarget(document)`: the console of the document's window, or
  `nullptr`); `ctx.loggedProblems()` returns the
  warnings and errors. `collectCachedMessages(document)` reads the messages a document without a target
  logger has cached, and caches them again for its console.
- Exceptions are caught, the transaction is cancelled, and the call returns `INTERNAL_ERROR`. A server
  failure never crashes the editor (PRD 7.4).
- `Oneshot` makes the intermediate state unobservable; since everything is synchronous, no repaint happens
  between do and rollback, so a dry run is invisible.

Tools that manage the history themselves (`undo`, `redo`, `transaction_*`, `command_repeat`) are
`Mutation::Map` + `transactional(false)`: they go through the busy gate and get a change report, but honor
`dryRun` themselves. `CallContext::setUndoStep()` names the undo step they created.

### 6.2 Explicit agent transactions (X3)

`transaction_begin{name}` calls `map.startTransaction("AI: " + name, LongRunning)` and stores it in the
document's `DocumentState` with its owning session. Later calls nest their `Oneshot` transactions; a failed
call rolls back only itself. `transaction_commit` → `commitTransaction()`, `transaction_rollback` →
`cancelTransaction()`. Rules:

- At most one agent transaction per document. Another session's modifying call on that document fails with
  `TRANSACTION_ACTIVE`, naming the owning client.
- `undo`/`redo` while one is open → `TRANSACTION_ACTIVE` (`CommandProcessor::undo()` requires an empty
  transaction stack).
- Calls inside the transaction report `undoStep: null` (the call runner reports a step only when it opened
  its transaction at depth 0), since their changes become part of the transaction's step.
- The call runner merges the change report of every successful, non-dry-run call made while the transaction
  is open (net: created then removed drops out, created then modified stays created), keyed by the
  document state and the transaction; `transaction_commit` reports these net `changes` and names the step
  in `undoStep`. Issues introduced are reported by each call, not again on commit.
- Session DELETE, disconnect, **Stop agent**, or closing the document → rollback.
- The status bar shows "AI transaction open: <name>". Human edits made meanwhile become part of the agent
  transaction; the manual section (E15.5) documents this.

### 6.3 Change report (`ChangeCollector`, X5)

| Notifier | Action |
|---|---|
| `nodesWereAddedNotifier` | add nodes and descendants to `added` |
| `nodesWillBeRemovedNotifier` | format their ids before removal (canonical ids) |
| `nodesWereRemovedNotifier` | add them to `removed` |
| `nodesWillChangeNotifier` | snapshot the issue signatures of those nodes |
| `nodesDidChangeNotifier` | add them to `changed` |
| `nodeVisibilityDidChangeNotifier`, `nodeLockingDidChangeNotifier` | add them to `changed` (`stateOnly`) |
| `selectionDidChangeNotifier` | mark the selection dirty |
| `currentLayerDidChangeNotifier`, `groupWasOpened/ClosedNotifier` | record `context` changes |

Reduction: `created = added − removed`, `removed = removed − added` (minus linked aliases),
`modified = (changed ∪ aliased) − created − removed`, including parents whose child sets changed. Issues
introduced: for `created ∪ modified`, `node->issues(validators)` after the call minus the `(type,
description)` signatures captured before. A dry run's report comes from the rolled-back execution and
excludes linked-group propagation (performed by `Map::commitTransaction`); if it created objects,
`changes.ephemeral` is `true` because those ids will not exist.

Result envelope of modifying tools (tool data under `result`):

```json
{
  "ok": true, "dryRun": false, "undoStep": "AI: Create Box Brush",
  "result": { "brush": "brush:1042" },
  "changes": { "created": ["brush:1042"], "modified": ["layer:3"], "removed": [] },
  "selection": { "mode": "objects", "count": 1, "ids": ["brush:1042"], "truncated": false },
  "issuesIntroduced": [],
  "warnings": [],
  "grid": 16
}
```

Each `issuesIntroduced` item is `{objectId, type, description, code, source}`: `source: "editor"` items come from
the editor's validators (`code` is the validator name in UPPER_SNAKE case), `source: "mcp"` items from the MCP
placement checks (`PlacementChecks.h`) and carry `details`:

| Code | Object | `details` |
|---|---|---|
| `Z_FIGHTING` | a face | `faces`, `brushes`, `materials`, `area`, `center`, `plane` |
| `ENTITY_OUTSIDE_HULL` | a point entity | `classname`, `position`, `gap` (bounds), `gapBrushes`, `cellSize` |
| `MODEL_BELOW_FLOOR`, `MODEL_FLOATING`, `MODEL_PENETRATES_BRUSHES`, `MODEL_NO_FLOOR` | a point entity | `modelBounds`, `surface`, `relatedIds`, `distance`, `suggestedMove` |
| `UV_ASPECT_DISTORTION` | a face | `face`, `brush`, `material`, `measured`, `fix` |

**Placement tracking.** A `PlacementTracker` inside the `ChangeCollector` snapshots the MCP findings of nodes in
`nodesWillChange` / `nodesWillBeRemoved` and diffs them against the findings after the call (`sameIssue`: by
signature, but face issues match despite a slightly moved or split face — Snap Vertices — when code, brushes (and
material) agree, the normals differ by at most 10° and z-fighting planes by at most 1 unit):
z-fighting for pairs involving created or modified brushes (signature: both brush ids and the plane); model
placement for created or modified point entities and entities next to changed brushes (for entities without a
snapshot only findings that name a changed brush count); UV aspect distortion for the faces of created or modified
brushes (a cheap pre-filter skips square-texel faces whose material has neither a note nor corpus statistics).
Findings involving created objects are always introduced. An MCP issue is dropped when the call already warned
the same code for the same object (the model warnings of `entity_create_point`, `objects_move`,
`entity_animation_set`; the UV warnings of the material, face and `uv_align` tools).

**Leak cache.** Leak prediction is global, so the per-document `PlacementCache` (in `DocumentState`) keeps a change
counter (never decreasing, also on undo), the leak signatures of the last analysis and the key they belong to; a
rollback restores the key. Prediction runs only when brush, entity, patch or group ids changed; the "before"
result is computed lazily before the first change of a call, and only when the cache is stale. While no entity is
enclosed (an unfinished map), only entities next to a gap are reported. If one prediction takes more than 500 ms
or the grid would be too large, leak checks are turned off for that document with one `LEAK_CHECK_SKIPPED`
warning. Measured in Debug on 2,000 brushes: `brush_create_box` 1.3 ms without and 18.5 ms with the checks (almost
all of it `predictLeaks`), moving 400 brushes 288 ms / 536 ms.

`Mutation::External` tools omit `changes`, `selection` and `issuesIntroduced`. Read-only tools return the
handler's JSON as `structuredContent` (plus `warnings` if any). Change lists are capped at 500 ids each
(`truncated: true` plus counts).

### 6.4 Selection independence (X7): select, act, restore

Most `Map_*` mutators act on the current selection. `Targets.h`:

```cpp
schema::Field idsField(std::vector<ObjectKind> kinds = {}, std::string description = ...);
Result<std::vector<mdl::Node*>, ToolError> resolveTargets(CallContext&, const Args&,
                                                          std::string_view key = "ids",
                                                          const std::vector<ObjectKind>& kinds = {});
Result<mdl::BrushFaceHandle, ToolError> resolveFace(CallContext&, std::string_view id);
ToolResult withTargets(CallContext&, const std::vector<mdl::Node*>& targets,
                       const std::function<ToolResult()>& fn,
                       SelectionAfter after = SelectionAfter::Restore);

schema::Field faceTargetsField(std::string description = ...);
Result<std::vector<mdl::BrushFaceHandle>, ToolError> resolveFaceTargets(CallContext&, const Args&,
                                                                        std::string_view key = "ids");
ToolResult withFaces(CallContext&, const std::vector<mdl::BrushFaceHandle>& faces,
                     const std::function<ToolResult()>& fn);
```

`resolveTargets` uses the given ids or, without ids, the current selection (`NO_SELECTION` if empty).
Targets resolve to what the editor would select and transform: a group stays a group (it is transformed as a
whole), and the id of a brush entity stands for its brushes and patches, because the editor never selects a
brush entity itself (`EditorContext::selectable` is false for entities with children); clicking one selects
its brushes. Tools that accept brushes or patches but not entities keep only the members of those kinds, and
`idsField` then accepts entity ids in the schema and says so in the description; a point entity (or a brush
entity without such members) fails with `WRONG_OBJECT_KIND`. Non-editable targets (hidden, locked, inside a
closed group) fail with `OBJECT_NOT_EDITABLE`, naming the brush entity if one of its members is not editable;
the hint names the fix (`layer_set_state` unlock/show, or `group_open`). Other explicit ids of the wrong kind
fail schema validation (`INVALID_ARGUMENT`); a selection of the wrong kinds gives `WRONG_OBJECT_KIND`.
`selection_set` expands brush entity ids the same way.

`withTargets` saves the selection, selects exactly the targets, runs `fn`, and restores the saved selection
(dropping removed nodes), all inside the call transaction. Tools whose editor counterpart leaves results
selected use `SelectionAfter::Result`: creation, duplicate, array, clip, extrude-to-new, all `csg_*`, and
(later) paste, import and group.

Face tools take `faceTargetsField()` ids: face ids (`brush:12/face:3`) and brush, group or entity ids, which
stand for all faces of the brushes they contain. `resolveFaceTargets` returns them without duplicates in id
order; without ids it uses the selected faces, else all faces of the selected objects (`NO_SELECTION` if that
yields no face). Faces that cannot be selected fail with `OBJECT_NOT_EDITABLE`. `withFaces` selects exactly
these faces (the `Map_Brushes` UV functions act on `selection().allBrushFaces()`), runs `fn` and restores the
saved selection, sharing the save/restore code with `withTargets`.

### 6.5 Non-map mutations

`Mutation::External` tools (save, game path, grid, locks, and later camera, preferences, compile) are not
undoable and get no transaction. They honor `ctx.dryRun()` by validating and describing the effect (e.g.
`"wouldDo": "overwrite /maps/a.map"`) without side effects.

### 6.6 TbMdlLib support

- `Node::runtimeId()` (§5.2).
- `CommandProcessor::transactionDepth()` (busy gate; the server reads it through `Map::commandProcessor()`).
- `CommandProcessor::undoCommandNames()` / `redoCommandNames()`, most recent first (`history_get`,
  `undo`/`redo`).
- The redo stack is cleared only when a command or transaction reaches the top-level undo stack
  (`storeCommand` / `createAndStoreTransaction`), so rolling back a transaction (dry run, failed call) keeps
  the human's redo history.
- `Map::canRedoCommand()` checks `redoCommandName()`.

Hollowing with a wall thickness is `mcp::csgHollow(Map&, double thickness)` (`include/mcp/tools/CsgUtils.h`),
the editor's `csgHollow` with the thickness as a parameter; thickness ≤ 0 fails.

---

## 7. Tool definitions, errors, pagination, naming

### 7.1 Naming

- Tool names are the spec's names verbatim: `snake_case`, `domain_verb[_object]`, `^[a-z][a-z0-9_]{0,63}$`.
- `title` is short Title Case ("Create Box Brush"), used for the undo name `AI: <title>`, the status bar
  and the call log.
- Common argument names: `ids`, `faces`, `document`, `dryRun`, `cursor`, `limit`, `fields`, `detail`.
  Coordinates are `position`/`min`/`max`/`center`/`vector`; angles `angle`/`angles` in degrees; lengths in
  map units. All paths are absolute.
- Prompt names: `blockout_level`, `populate_level`, `lighting_pass`, `texture_pass`, `fix_issues`,
  `compile_and_debug`, `explain_map`, `explain_entity`, `cleanup_map` (§8).

### 7.2 `ToolDef` and the schema builder (`ToolRegistry.h`, `Schema.h`)

A value-type DSL builds a schema tree that is both the JSON Schema published in `tools/list` and the
validator/decoder of incoming arguments (fills in defaults, rejects unknown properties with the list of
allowed ones). There is no generic JSON Schema validator: we accept only what we declare.

```cpp
using namespace tb::mcp::schema;

void registerGeometryTools(ToolRegistry& registry)
{
  registry.add(ToolDef{"brush_create_box"}
    .title("Create Box Brush")
    .description("Creates a cuboid brush spanning min..max (map units, Z up). Example: "
                 "{\"min\":[0,0,0],\"max\":[256,256,16],\"material\":\"base_floor\"}")
    .input(object({
      field("min", vec3()).required().describe("Minimum corner"),
      field("max", vec3()).required().describe("Maximum corner; each component > min"),
      field("material", string()).describe("Material name; default: current material"),
    }))
    .output(object({field("brush", objectId({ObjectKind::Brush}))}))
    .mutation(Mutation::Map)          // injects `dryRun` and `document`
    .handler(createBox));             // ToolResult createBox(CallContext&, const Args&)
}
```

- Schemas: `any() boolean() integer() number() string() enumOf({...}) array(T) object({...}) oneOf({...})
  vec3() vec2() box() angle() objectId(kinds) documentId()`; modifiers `describe defaultsTo min max minSize
  maxSize nonEmpty matching withFormat withCheck allowAdditionalProperties`; fields `field(name, schema)
  .required() .describe() .defaultsTo()`. Published output schemas do not contain
  `additionalProperties: false`.
- `ToolDef`: `title description input output mutation documentUse transactional paginated destructive
  idempotent openWorld keepsActiveTool handler asyncHandler`. `keepsActiveTool` makes a `Map` tool skip
  `prepareForAgentEdit` (§4.1), so it runs in the editor's current tool state (`action_invoke`). Annotations: `readOnlyHint` (Mutation::None),
  `destructiveHint`, `idempotentHint`, `openWorldHint`.
- `Mutation`: `None` (read-only, runs immediately), `Map` (one transaction, busy gate), `External`
  (non-undoable side effects, busy gate).
- `DocumentUse`: `None` (no `document` parameter), `Optional`, `Required` (`NO_DOCUMENT` without a target
  document). Default: `Required` for `Mutation::Map`, `None` otherwise.
- Injected parameters: `document?` for `DocumentUse != None`; `dryRun? = false` for `Map`/`External`;
  for `.paginated()`: `cursor?`, `limit? = 100 (1..1000)`, `fields?`, `detail? = "summary"|"full"`.
- `CallContext`: `server() host() session() tool() hasDocument() documentInfo() document() map()
  documentState() ids() dryRun() warn() warnings() addImage() progress() setUndoStep() loggedProblems()
  cancelled() defer() operationFailed()`. `addImage(bytes, mimeType)` appends an MCP `image` content block
  (base64) after the text block that holds the structured result (`material_preview`). `Args::get<T>` uses the `JsonVm.h` converters; a handler never sees an
  unvalidated value.

### 7.3 Errors (`Errors.h`, X9)

```cpp
struct ToolError {
  ErrorCode code;                     // serialized as UPPER_SNAKE string
  std::string message;                // one sentence
  std::vector<std::string> objectIds; // involved objects
  std::string hint;                   // concrete next step
  Json details = Json::object();      // e.g. schema error path, editorMessages
};
struct Warning { std::string code; std::string message; std::vector<std::string> objectIds; };
using ToolResult = Result<Json, ToolError>;
```

Codes: `INVALID_ARGUMENT`, `OBJECT_NOT_FOUND`, `WRONG_OBJECT_KIND`, `OBJECT_NOT_EDITABLE`, `NO_SELECTION`,
`NO_DOCUMENT`, `DOCUMENT_NOT_FOUND`, `INVALID_GEOMETRY`, `OUT_OF_WORLD_BOUNDS`, `OPERATION_FAILED`,
`TRANSACTION_ACTIVE`, `NO_TRANSACTION`, `BUSY_TIMEOUT`, `CANCELLED`, `UNSAVED_CHANGES`, `FILE_EXISTS`,
`IO_ERROR`, `UNSUPPORTED` (game/format), `UNSUPPORTED_IN_HOST`, `DRY_RUN_UNSUPPORTED`, `COMPILE_RUNNING`,
`DIALOG_REQUIRED`, `ACTION_REFUSED` (§10.9), `INTERNAL_ERROR`.

Mapping to MCP:
- Tool failures, including argument validation failures, are `CallToolResult` with `isError: true`,
  `structuredContent: {"ok":false,"error":{...}}` and a text block such as `"INVALID_GEOMETRY: brush:12 would
  become non-convex. Hint: use a smaller offset."`, so the model can self-correct.
- JSON-RPC errors only for protocol faults: `-32700` parse, `-32600` invalid request, `-32601` unknown
  method, `-32602` unknown tool / bad `params`, `-32603` internal, `-32002` resource not found.
- Warnings (X14) never fail a call; they go to `warnings: [{code, message, objectIds}]`. Codes are
  UPPER_SNAKE strings defined by the tools (§10).

### 7.4 Pagination and fields (X10)

- `cursor` is opaque: base64 of `{"o":<offset>,"m":<modificationCount>}`. If the document's
  `modificationCount` changed since the cursor was issued, the page is still served with `"stale": true`.
- A list response is `{"items":[...], "total":N, "nextCursor":"..."|null}`.
- `fields` selects top-level keys and dotted paths (`"faces.material"`). `detail:"summary"` returns each
  tool's compact shape. `object_get` warns `UNKNOWN_FIELD` for paths that none of the returned objects has
  (`unknownFields`); a path that some objects have (e.g. `origin` for a point entity and a brush) is not
  unknown.
- `tools/list` pages at 1,000 (in practice one page); `resources/list` and `prompts/list` page at 100.

---

## 8. Resources and notifications (`Resources.cpp`)

| URI | Content | Updated when |
|---|---|---|
| `trenchbroom://editor/status` | editor status | documents open/close/focus change; info or selection changes, grid, tool changes, lock preferences (`AlignmentLock`, `UvLock`), agent transactions opening or closing |
| `trenchbroom://documents/{doc}/info` | `documentInfo()` | save, load, modified flag flips, mods, entity definitions, materials or worldspawn change |
| `trenchbroom://documents/{doc}/summary` | `mapSummary()` (= `map_summary`) | nodes added/removed/changed, visibility, locking, current layer, grid, entity definitions, reload |
| `trenchbroom://documents/{doc}/selection` | `selectionDetails()`, ≤ 100 items | selection changes, changes of selected nodes |
| `trenchbroom://documents/{doc}/entity-definitions` | all classes of the document (per document: definitions depend on its mods and definition file) | definitions reloaded |
| `trenchbroom://documents/{doc}/materials` | `materialsResource()`: collections `{path, materialCount}` and loaded materials `{name, collection, width, height}` (no usage counts, which change on every edit) | material collections changed, document loaded, material images processed |
| `trenchbroom://games/{game}/config` | `gameConfigJson()`; `{game}` percent-encoded; listed for games of open documents | `game_set_path` |
| `trenchbroom://compile/{run}/log` | the full log of a compile run as `text/plain` (`{run}` is e.g. `run:3`); listed once per known run | output appended, run ended |
| `trenchbroom://documents/{doc}/issues` | `issuesResource()` (`ValidationTools.h`): the issues `issues_list` returns without filters (hidden issues and turned-off validators excluded), `total`, `counts`, `truncated`, at most 200 items, `leakCheck`, `disabledValidators` | whenever the summary is updated (objects added, removed or changed, entity definitions, reload), issues hidden or shown, validators turned on or off |
| `trenchbroom://console` | the newest console messages (§9.1) | new messages (250 ms coalescing), clear |
| `trenchbroom://guide` | agent guide as `text/markdown` (`AgentGuide` raw string in `Resources.cpp`, `agentGuide()`): conventions (units, axes, yaw, ids, grid), the player dimensions per game family (the `playerSize()` table of `AgentCamera` and the `walkable_plan` defaults), the workflow, checks and pitfalls. Tool names are in backticks | static |
| `trenchbroom://manual` | table of contents of the user manual: `{title, sectionCount, sections[{id, title, level, parent, uri}]}` (§10.16) | static |
| `trenchbroom://manual/{section}` | one manual section as `text/markdown` (`{section}` is a section id), with links to its subsections; not listed per section | static |

Templates are listed once per open document. `DocumentState` reports `DocumentAspect::{Info, Summary,
Selection, EntityDefinitions, Materials, Issues, Status}` changes; every summary update also schedules the issues
resource. Info notifications and document open/close are immediate; all other updates go
through `ServerState::scheduleResourceUpdate` / `scheduleDocumentUpdate`, coalesced into one
`notifications/resources/updated` per resource and scheduler turn. Nothing is recorded while no session has
subscriptions (the hooks run on every map change, e.g. during drags).

`trenchbroom://console` (`ConsoleTools.cpp`) holds the newest 200 console messages of level info or higher,
`lastSeq`, `buffered` and `capacity`. `ServerState` watches the buffer's `messagesAddedNotifier` and
`clearedNotifier` and sends one `resources/updated` per burst, 250 ms after the first message; nothing is
scheduled without subscribers. Lines that start with `[AI] ` (the call log sink, §9) do not notify, so a
client that answers notifications with calls cannot loop.

### Prompts (`Prompts.cpp`)

`registerPrompts` adds the task templates of spec §22 to the `PromptRegistry`. Each returns one user
message that walks the agent through the tools in order, with their key arguments, the checks to run and
what to report; it tells the agent to read the guide first and to learn the game's classes and flags
instead of assuming Quake. Argument values are inserted into the text; missing or blank optional
arguments get defaults. Tool names are written in backticks and nothing else is; `tst_Prompts` checks that
every backticked name (in the prompts and in the guide) is a registered tool and that no other word names
one.

| Prompt | Arguments (required in bold) | Guides the agent to |
|---|---|---|
| `blockout_level` | **description**, game, style | set up the document (`document_new`, `document_save_as`, materials), build rooms with `room_create` and `opening_cut` at the game's player scale, place the player start, check with `spaces_list`, `walkable_plan`, snapshots, `map_check`, a fast compile, record the manifest |
| `populate_level` | difficulty (easy, normal, hard, all), theme, spaces | read the game's monster and item classes and difficulty spawnflags, place them with `free_spots` and `entity_create_point` (dropToFloor), set flags, check placement |
| `lighting_pass` | mood, spaces | learn the game's light keys, place lights per room in a Lights layer, check, compile to judge |
| `texture_pass` | theme, materials | choose one material per surface type (`material_preview`, `material_usage`), apply and align (`uv_align` typical), `uv_check` |
| `fix_issues` | scope (safe, all) | collect `issues_list`, `map_check`, broken links, explain them, `issue_fix` with dry runs (safe: no deletions except empty objects), fix Z_FIGHTING and leaks by hand, report what remains |
| `compile_and_debug` | preset (fast, normal, full), profile | save, check the tools, `compile_run`, poll `compile_status`, find leaks with `pointfile_load`, offer `engine_launch` |
| `explain_map` | focus | summarize layout, entities, gameplay flow and problems without changing the map |
| `explain_entity` | **classname** | describe a class from `entity_class_describe` and its use in the map |
| `cleanup_map` | format (map, obj or a map format), exportPath | remove empty objects, fix properties, links, vertices and materials, export (`document_export_map`, `document_export_obj`, or `document_new` + `map_import` for another format) |

---

## 9. Call log and observability

`CallLog` records every call: `{seq, time, session, client, tool, argsDigest, durationMs, ok, errorCode,
undoStep, change counts}`; arguments up to 4 KB, then truncated; ring buffer of 5,000 entries. `session_log`
lists newest first; its cursors are tied to the last log sequence number, so a page requested after new
calls is `stale`. Sinks:

1. Console: `[AI] <tool> ok 3 ms (+1 ~2 -0)` (or the error code) to the top map window's logger.
2. `JsonlFileSink` when `McpLogToFile` is set: `<user data folder>/mcp-logs/<yyyyMMdd-HHmmss>-<pid>.jsonl`,
   rotated at 10 MB with `.1`, `.2`, … suffixes.

The raw JSON-RPC trace is logged only at debug level.

### 9.1 Editor console (`ConsoleBuffer`, `McpConsoleHook`, `ConsoleTools.cpp`)

The console buffer holds every message that any `ui::Console` (the console tab of a map window) logs, from
editor start: load errors of materials, models and entity definitions, save/export messages, the `[AI]`
call log lines, and so on.

- **Capture.** `Console::messageLoggedNotifier` (§15) fires synchronously in `Console::doLog`, which may run
  on worker threads; the calls are serialized, and messages logged by an observer are not reported (a
  `thread_local` guard). `McpConsoleHook` adds main-thread messages at once, so messages logged during a
  synchronous call are in the buffer before the call returns; messages from worker threads are queued to the
  main thread. The document is resolved from the console's window (`MapWindow::document()`); the buffer
  stores the document pointer (only compared with open documents) and its file name.
- **`ConsoleBuffer`** (TbMcpLib, main thread only): 10,000 messages; each gets a sequence number that is never
  reused, also not after `clear()`; the oldest message is dropped when full.
- **Per-call report (E10.13).** `CallRunner` remembers the last sequence number when a call starts and adds
  the warnings and errors logged meanwhile (any document, at most 50 plus a note) to the result as
  `console: [{seq, level, text, document | documentName}]` — also to error results and to asynchronous
  calls; omitted when empty.
- **`console_read`** (read-only; no default document): `minLevel` (default `info`), `text` (case-insensitive
  substring, or an ECMAScript regex with `regex: true`), `document` (`doc:<n>`, a closed document's file name,
  or `none`); `after` (a sequence number, typically the previous `lastSeq`), `cursor`/`limit` (1–1000, default
  100), `newest`; pages are oldest first. Returns `{items: [{seq, level, time, text, document | documentName}],
  total, nextCursor, lastSeq, dropped?}`; `dropped` counts messages after `after`/`cursor` that were lost to
  the buffer limit or a clear.
- **`console_clear`** (`Mutation::External`, honors dry run): clears the buffer and every console view
  (`Console::clear`, §15) through `McpHost::clearConsoleViews`.
- Without a console buffer (`host.consoleBuffer() == nullptr`) the console tools fail with
  `UNSUPPORTED_IN_HOST`. `FakeHost` has `console`, `supportsConsole` and `clearConsoleViewsCount`; its
  `logTarget()` is a per-document logger that adds to the buffer and is also set as the document's logger when
  a document is added, like a map window's console.

---

## 10. Tools

### 10.1 Source layout (one file per domain, `src/tools/`)

| File | Tools | Epic |
|---|---|---|
| `SessionTools.cpp` | `editor_status`, `document_list`, `document_activate`, `session_log` | E1 |
| `HistoryTools.cpp` | `history_get`, `undo`, `redo`, `transaction_begin/commit/rollback` | E1 |
| `DocumentTools.cpp` | `document_new/open/save/save_as/close/revert/recent`, `map_files_list`, `document_export_map/obj`, `autosave_list` | E2 |
| `GameTools.cpp` | `game_list`, `game_info`, `game_set_path`, `mods_get/set`, `entity_definitions_get/set/reload`, `materials_collections_get/set`, `materials_reload`, `soft_bounds_get/set` | E2 |
| `SceneTools.cpp` | `map_summary`, `map_tree`, `object_get`, `objects_find`, `map_text_get`, `map_stats` | E3 |
| `SpatialTools.cpp` | `objects_at_point`, `ray_pick`, `space_check`, `map_plan_view` (image form: E10) | E3, E10 |
| `SelectionTools.cpp` | `selection_get/set/clear`, `select_all`, `select_invert`, `select_by`, `select_spatial`, `select_siblings`, `select_by_line`, `select_faces_of` | E3 |
| `GeometryTools.cpp` | `brush_create_box/shape/hull`, `room_create`, `opening_cut` | E4 |
| `BrushEditTools.cpp` | `brush_clip`, `face_extrude`, `face_extrude_new`, `vertices_move/remove/snap`, `vertex_add`, `csg_merge/subtract/intersect/hollow` | E4 |
| `TransformTools.cpp` | `objects_move/rotate/scale/shear/flip/duplicate/delete/array`, `command_repeat`, `command_repeat_clear` | E4 |
| `ViewTools.cpp` | `grid_get/set`, `camera_get/set/focus/step_pointfile`, `view_options_get/set`, `view_layout_set` | E4, E14 |
| `MaterialTools.cpp` | `materials_list`, `material_apply`, `material_set_current`, `material_replace`, `material_preview`, `locks_get/set` | E4, E6 |
| `FaceTools.cpp` | `face_attributes_get/set/copy`, `uv_align`, `uv_nudge` | E6 |
| `TagTools.cpp` | `tags_list`, `tag_apply`, `tag_remove` | E6 |
| `EntityClassTools.cpp` | `entity_classes_list`, `entity_class_describe`, `entity_model_info` | E5 |
| `EntityCreateTools.cpp` | `entity_create_point`, `entity_create_brush`, `entity_move_brushes` | E5 |
| `EntityPropertyTools.cpp` | `entity_properties_set`, `entity_property_remove/rename`, `entity_spawnflags_set`, `entity_defaults_apply`, `entity_links_get`, `entity_link`, `entity_color_set` | E5 |
| `CompileTools.cpp` | `compile_tools_get/set`, `compile_presets_list`, `compile_profiles_list`, `compile_profile_save/delete`, `compile_run`, `compile_status`, `compile_cancel`, `pointfile_load/unload`, `portalfile_load/unload`; the compile log resource | E7 |
| `SnapshotTools.cpp` | `agent_camera_set/get/list/delete`, `view_snapshot`, `view_snapshots_around`, `view_snapshot_compare`, `view_snapshot_user` | E10 |
| `ConsoleTools.cpp` | `console_read`, `console_clear`; the console resource | E10 |
| `LayerTools.cpp` | `layers_list`, `layer_create/rename/remove/reorder`, `layer_set_state`, `objects_move_to_layer`, `visibility_set` | E9 |
| `GroupTools.cpp` | `group_create/ungroup/rename`, `groups_merge`, `group_add_objects/remove_objects`, `group_open/close`, `linked_group_duplicate/select/separate/extract` | E9 |
| `ClipboardTools.cpp` | `clipboard_copy/cut/paste`, `map_file_inspect`, `map_import` | E9 |
| `MaterialKnowledgeTools.cpp` | `material_corpus_scan`, `material_notes_get/set`, `material_usage` | E11 |
| `UvTools.cpp` | `uv_check`, `material_fit_geometry` | E11 |
| `EntityModelTools.cpp` | `entity_animation_set`, `entity_placement_check` | E11 |
| `PickTools.cpp` | `view_pick` | E12 |
| `SpaceTools.cpp` | `spaces_list`, `surroundings`, `free_spots`, `walkable_plan` | E12 |
| `ManifestTools.cpp` | `map_manifest_get`, `map_manifest_set` | E12 |
| `ValidationTools.cpp` | `issues_list`, `issue_fix`, `issue_hide`, `issue_show`, `validators_list`, `validators_set`; `issuesResource()` for the issues resource | E12 (E13.1), E13 |
| `MapCheckTools.cpp` | `map_check` | E13 |
| `EngineTools.cpp` | `engine_profiles_list`, `engine_profile_save`, `engine_launch` | E13 |
| `ActionTools.cpp` | `actions_list`, `action_invoke` | E14 |
| `PreferenceTools.cpp` | `preferences_get`, `preferences_set` | E14 |
| `KnowledgeTools.cpp` | `manual_search`, `manual_section`; the manual resources | E14 |

The prompts are in `src/Prompts.cpp` (§8).

`CompileTools.h` also declares `registerCompileResources`. Each domain header `include/mcp/tools/<Domain>Tools.h` declares `register<Domain>Tools` and the helpers
shared with resources: `documentInfo()` (DocumentTools.h); `gameConfigJson()`, `modsJson()`,
`entityDefinitionsJson()`, `materialsJson()`, `softBoundsJson()` (GameTools.h); `mapSummary()`
(SceneTools.h); `selectionDetails()` (SelectionTools.h); `materialsResource()` (MaterialTools.h);
`materialKnowledge()` and `warnKnowledgeProblems()` (MaterialKnowledgeTools.h).

### 10.2 Shared helpers (`src/tools/`)

Each helper takes the narrowest context it needs, in this order: the node (or nodes) it works on,
then `mdl::Map&` (or `const mdl::Map&`), then `IdRegistry`, and `CallContext` only when it warns,
reads the call's logged problems or changes the selection for the call. No helper takes
`ui::MapDocument`, so the model helpers run over a plain `mdl::Map` (`mdl::MapFixture` in tests).

- **`ToolUtils.{h,cpp}`**: game lookup (`findGame`, `gameNames`, `unknownGameError`, `gamePath`,
  `isGamePathValid`), ISO times, `absolutePathArgument` (`INVALID_ARGUMENT` for relative paths),
  `toJson(LogMessage...)`, percent-encoding, `pathExists`.
- **`NodeJson.{h,cpp}`**: `nodeSummary` (`{id, kind, label, bounds, layer, classname | name | materials,
  entity}`; every list item that describes an object uses it), `nodeState`, `faceJson` (every face),
  `nodeLabel`, `nodeMaterials`, tag names, `layerIdOf`, `groupIdOf`.
- **`GeometryUtils.{h,cpp}`** (header in `include/mcp/tools/`; `tst_GeometryUtils` tests the model
  helpers directly over `mdl::MapFixture`): `brushBuilder` (game face defaults), `materialArgument`
  (`UNKNOWN_MATERIAL` warning), `checkBox`, `checkInsideWorldBounds`, `geometryError` /
  `geometryOperationFailed`, `addBrushes`, `nodeSummaries`, `formatIds`, `warnNonIntegerVertices`
  (`NON_INTEGER_VERTICES`), `ScopedLockOverride` (per-call `alignmentLock` / `uvLock`),
  `intersectsInterior` (exact brush/box overlap), `classifyBrush`, `owningBrushEntity`, `isPointEntity`,
  `castRay`. `CallContext` remains only in `materialArgument` and `warnNonIntegerVertices` (they warn)
  and `geometryOperationFailed` (it reads the logged problems).
- **`EntityUtils.{h,cpp}`**: definition lookup, property type names and definition JSON, color ranges, flag
  lookup by name, bit (`bit8`) or value, value validation (`checkPropertyValue`, `validateProperty`,
  `warnUnknownClassname`), entity targeting (`resolveEntities` over `mdl::Map` and `IdRegistry`;
  `withEntities`). `validateProperty` and `warnUnknownClassname` take `CallContext` because they warn,
  `withEntities` because it selects through `withTargets`.
- **`AssetUtils.{h,cpp}`** (header in `include/mcp/tools/`): `createGameFileSystem` (the game path, the
  enabled mods and the WADs of the world's `wad` property, as the map mounts them; the map's own game file
  system is private), `loadMaterialImage` and `MaterialImageLoader` (mip 0 as RGBA from the texture's CPU
  buffers, or read from the game file system with the palette; the loader builds the file system once).
- **`MaterialKnowledge.{h,cpp}`**, **`UvCheck.{h,cpp}`** and **`EntityModelUtils.{h,cpp}`** (headers in
  `include/mcp/tools/`, tested directly): material profiles (§10.8), texturing checks (§10.8), entity
  models and placement checks (§10.7).
- **`SpaceAnalysis.{h,cpp}`** and **`PlacementChecks.{h,cpp}`** (headers in `include/mcp/tools/`, tested directly over
  `mdl::MapFixture`): the empty-space grid, spaces, free spots, walking and leak prediction; z-fighting and the
  placement tracker of the change report (§6.3, §10.13). They take `const mdl::Map&` and, at the JSON layer,
  `IdRegistry`.
- **`CompileUtils.{h,cpp}`** and **`CompileLog.{h,cpp}`** (headers in `include/mcp/tools/` so that
  `TbMcpLibTest` can test them directly): compile presets, tool path checks, profile JSON and schemas; log
  analysis (§10.10).

`brush_create_shape` (`GeometryTools.cpp`) builds its shapes with `mdl::BrushBuilder` (via `brushBuilder`)
and the requested material, using the same builder calls and the same step layout for stairs as the
editor's shape tool extensions (`ui::DrawShapeTool*Extension`). It does not use those extensions because
they take a `ui::MapDocument`; only the parameter type `ui::DrawShapeToolParameters` is shared with them.

### 10.3 Documents and games

- `unsavedChanges: "error" | "save" | "discard"` (default `"error"` → `UNSAVED_CHANGES`) on
  `document_close`, `document_revert`, and on `document_new` / `document_open` when they replace a document
  (single-window mode). `"save"` fails for a never-saved document. The server does not create folders.
- `document_new` / `document_open` make the result the session's active document. `document_new` reports the
  game's `initialMap` template for the format (or null) and the ids of the objects the new map starts with
  (`initialObjects`, e.g. the single default brush), with an `INITIAL_OBJECTS` warning, so that agents delete
  them before building at the origin.
- `document_open` reads game and format from the header comments; explicit `game` / `format` override them;
  a missing format is detected by the loader (`formatSource: "detected"`). An already open file is returned
  with `alreadyOpen: true`. `loadMessages` lists warnings and errors logged while loading.
- `document_revert` reloads from disk (`idsInvalidated: true`); `document_close` refuses while a compilation
  runs.
- `document_save_as` accepts the document's own path without `overwrite` and warns `UNUSUAL_EXTENSION` for
  non-`.map` paths. Exports refuse the document's own path.
- `map_files_list` matches a case-insensitive glob (default `*.map`), optionally recursive, sorted naturally.
  `autosave_list` lists `<map dir>/autosave/<name>.<n>.map`, newest first.
- `game_info` without `game` describes the active document's game; smart tags are reported with name,
  attributes and a textual definition.
- `mods_set`, `entity_definitions_set`, `materials_collections_set` and `soft_bounds_set` are
  `Mutation::Map` (worldspawn changes, one undo step each); reload problems become `LOAD_WARNING` /
  `LOAD_ERROR` warnings. Unknown values are warnings: `UNKNOWN_MOD`, `DEFAULT_MOD`, `FILE_NOT_FOUND`,
  `UNKNOWN_COLLECTION`, `BOUNDS_OUTSIDE_WORLD`.
- `materials_collections_set` takes `wads` (ordered WAD list; WAD games only, else `UNSUPPORTED`) and/or
  `enabled` (enabled collection paths). Relative WAD paths that are found (next to the map, in the game or
  application folder) are stored as absolute paths, as the editor's path dialog does by default, because
  compile tools such as hlcsg cannot open game-relative paths; `keepRelative: true` stores them as passed
  with a `RELATIVE_WAD_PATH` warning naming the absolute path. `entity_definitions_set` takes `type: "builtin" | "external"` and
  `path`. `soft_bounds_*` use `mode: "game" | "unlimited" | "custom"` with `bounds`.

### 10.4 Scene, spatial and selection

- `map_tree` returns the flattened depth-first tree as a page; nodes at the depth limit carry `descendants`
  counts by kind. `kinds` filters items, but containers are still traversed.
- `object_get` takes up to 50 object or face ids (default `detail: "full"`); one unknown id fails the call,
  listing all unknown ids. Point vs brush entity is decided by whether the entity has children.
- `objects_find` filters are AND-combined; globs (`*`, `?`) are case-insensitive; an unknown tag is a
  `UNKNOWN_TAG` warning. Pages carry `counts` by kind for the whole match set.
- `map_text_get` pages by lines (`startLine`, `maxLines` ≤ 5000); a layer id stands for its contents. Line
  numbers match the file on disk only right after loading or saving.
- `ray_pick` has its own loop over the world octree with the editor's face and entity hit tests (`mdl::pick`
  always applies the editor context and cannot include hidden objects). Faces are hit from the front only, so
  a ray starting inside a brush passes through it. `from: <id>` starts at the object's bounds center and
  ignores the object and its members. No hit is `hit: null`. Entity hits have no normal.
- `space_check` uses `intersectsInterior`, a separating-axis test (face planes, box axes, edge × axis) on the
  box shrunk by 0.01, because `Brush::intersects(bbox)` compares bounds only; touching surfaces do not
  overlap. With `solidOnly` (default) `trigger_*` brushes are ignored. Floor and ceiling come from five
  vertical rays (center and inset corners); `supportedCorners` counts corners with a surface within 1 unit.
- `map_plan_view` (text form; the image form is E10) classifies cells at the given height: `#` solid (world,
  `func_group`, `func_detail*`), `+` other brush entity, `t` trigger, `.` open with a floor within
  `floorDepth` (1024) below, space for void. The grid is aligned to multiples of `cellSize`; entity chars
  `P M I E L` in that priority. Patches count only as floor.
- Selection-changing tools are `Mutation::Map` (selection changes are undoable commands): one undo step each,
  dry run supported. `selection_get` is read-only and paginated. `selection_set` refuses to mix objects and
  faces (`INVALID_ARGUMENT`), world/layer ids (`WRONG_OBJECT_KIND`, hint: `select_by` layers) and
  non-selectable objects (`OBJECT_NOT_EDITABLE`, not checked in remove mode). `select_by` takes exactly one
  criterion; no match is `count: 0` plus a `NO_MATCH` warning. `select_faces_of` without `ids` or `face` uses
  the selected brushes; `coplanar` (default true) uses `collectConnectedCoplanarFaces`. Preconditions of the
  `Map_Selection` functions are checked before calling them.

### 10.5 Geometry

- **Validity (E4.16).** Degenerate or non-convex results are `INVALID_GEOMETRY`; results that reach or leave
  the world bounds are `OUT_OF_WORLD_BOUNDS` (`mdl::Brush` clips to the world bounds, so touching them counts).
  Both name the involved ids and the editor's logged message (`details.editorMessages`). Vertex, edge and
  face moves pre-check each brush (`canTransformVertices` etc.) to name the offending one. The editor logs
  nothing when a brush transform fails, so transform tools compute the transformed bounds themselves.
- **Creation** tools select their result. They build brushes with `mdl::BrushBuilder` and add them with
  `mdl::addNodes` to the current layer or open group. `brush_create_shape` calls the editor's
  `ui::DrawShapeTool*Extension` classes and applies `material` to the returned brushes' faces (the current
  material is unchanged). Parameters that do not apply to the chosen shape warn `IGNORED_ARGUMENT`, so their
  defaults are applied in code, not in the schema. The arch axis defaults to `x` (upright arch). Hollow
  cylinder and arch thicknesses are validated; a step height ≥ the box height warns `SINGLE_STEP`. The
  editor preference that groups shape brushes is not read; the `group` argument covers it.
- `brush_create_hull` explains degenerate point sets (coincident, collinear, coplanar) and warns
  `POINTS_INSIDE_HULL` for unused points.
- `room_create`: floor and ceiling span the outer footprint, west/east walls the outer depth, south/north
  walls fit between them; the result names each brush by role.
- `opening_cut` subtracts the opening from each target with `Brush::subtract`, re-applies the wall's own face
  attributes (subtract copies the cutter's attributes to coplanar faces), and uses `material` or the wall's
  most used material inside the opening. Any invalid fragment fails the call. Without ids it cuts every
  selectable brush the opening overlaps. Like the transform and face tools (`resolveTargets`), it does not
  cut brushes inside closed groups: that would edit a group's members outside the editor's open-group model
  (and bypass linked group propagation on close), so the error's hint names the closed groups to open.
- `brush_clip`: plane normal `cross(p1-p0, p2-p0)`; with 2 points `cross(b-a, axis)`; with `face` the face
  normal. "Front" is where the normal points.
- `face_extrude` groups faces by normal and extrudes each group. `face_extrude_new` reimplements the Extrude
  tool's split (outward, or inward for negative distances) and stamp logic with mdl calls, since those are
  file-local to `ExtrudeTool.cpp`.
- `vertices_move` and `vertices_remove` find the brushes of their handles (positions match within 0.01):
  explicit `ids` if given; else the selected brushes if they have every handle (a handle shared with an
  unselected brush then changes only the selected one, as in the editor); else every editable brush that has
  one of the handles, with a `HANDLES_OUTSIDE_SELECTION` warning if brushes are selected. Handles that are not
  found fail with `INVALID_ARGUMENT` (`OBJECT_NOT_EDITABLE` if only hidden or locked brushes have them); the
  message says where they were searched (the ids, the selection and the whole map with brush counts) and the
  hint names the brushes that have them.
- Subtracting with cutters that touch nothing and intersecting disjoint brushes follow the editor (brushes are
  removed) and warn `NOTHING_SUBTRACTED` / `EMPTY_INTERSECTION`. Other warnings: `NOTHING_CLIPPED`,
  `VERTICES_MERGED`, `SNAP_FAILED`, `NOT_HOLLOWED`.

### 10.6 Transforms, grid and locks

- Transforms use `withTargets` + `translateSelection` / `rotateSelection` / `scaleSelection` /
  `shearSelection` / `flipSelection`, so texture lock, entity angle updates and repeat semantics match the
  editor. Brush entity ids stand for their brushes and patches (§6.4), so a door or a button moves, rotates,
  duplicates (with a copy of the entity) or is deleted (with the entity) as a whole; results list the
  brushes, each with its `entity`.
- Rotate and flip default to the exact bounds center (the editor uses its grid reference point).
  `objects_rotate` sets the world's `updateAnglePropertyAfterTransform` for the call (`updateEntityAngles`,
  default true) and restores it.
- `objects_array`: `count` is the total number of instances including the originals (≤ 1024); circle arrays
  rotate copies around the center (`rotate`, default true), and `rise` offsets each instance along the axis
  (spiral stairs). The array is left selected; the call is one undo step and one repeatable entry.
- `command_repeat` is `transactional(false)`: the repeat stack refuses to repeat inside a map transaction,
  so the tool opens its own `LongRunning` command-processor transaction `AI: Repeat Last Commands`, handles
  dry run itself (rollback; empty change report, the result lists the selection), and fails with
  `TRANSACTION_ACTIVE` inside an agent transaction. Each agent call is one repeatable entry.
- `grid_set` and `locks_set` are `Mutation::External` (grid on the map; locks via the `AlignmentLock` /
  `UvLock` preferences, which `MapDocument` applies to the editor context).

### 10.7 Entities

- Classes are looked up in the document's `EntityDefinitionManager`, so FGD, DEF and ENT behave the same.
- Validation (X14) produces warnings that never block: `UNKNOWN_CLASSNAME`, `UNKNOWN_PROPERTY`,
  `INVALID_PROPERTY_VALUE`, `INVALID_CHOICE`, `UNKNOWN_FLAGS`, `READ_ONLY_PROPERTY`. Keys starting with `_` and
  well-known keys (`origin`, `angle`, `target`, …) are not reported as unknown. Entities of unknown classes
  are still created.
- Targeting: `ids` of entities, `world` (worldspawn) or brushes (meaning their entity); without ids the
  selection's entities, `NO_SELECTION` when nothing is selected. `withEntities` selects exactly the targets
  before calling the `Map_Entities` functions (which act on `selection().allEntities()`); worldspawn runs
  separately because the editor drops it from mixed selections.
- `entity_create_point` and `entity_create_brush` take `properties` like `entity_properties_set`: a `null`
  value removes the key. Removals run after the other properties and after `applyDefaults`, so they also
  remove defaults. Games whose configuration sets default properties (`setDefaultProperties`, e.g. Half-Life)
  get all defaults of the definition when the editor creates the entity, including empty ones such as a
  `func_breakable`'s `gibmodel`; the server keeps this editor behavior (the definition asks for these keys)
  and `null` removes such a key in the same call.
- `entity_create_point` snaps to the grid. `dropToFloor` casts five vertical rays (bounds center and inset
  corners, from the height of the bounds center) against visible solid and brush-entity brushes and patches
  (not triggers) and places the bounds on the highest hit. The dropped origin z is kept integral (model bounds
  are often fractional): within 0.01 of an integer it is rounded, otherwise rounded up so the bounds float less
  than 1 unit above the floor instead of sinking into it; x and y are unchanged. It warns
  `ENTITY_OVERLAPS_BRUSHES` if the bounds intersect brushes (`intersectsInterior`).
- **Model-aware placement (E11).** `EntityModelUtils` loads entity models synchronously with
  `mdl::loadEntityModelSync` from `createGameFileSystem` when the editor has not loaded them yet (the editor
  loads models asynchronously and processes them per frame, so right after `entity_create_point` a model is
  usually not loaded). The entity's own loaded model is preferred; an `EntityModelLoader` caches loads per call
  by path, failures included, and uses 1×1 placeholder skins because only geometry is needed. World bounds apply
  the entity's model transformation (origin, rotation with the model's pitch type, scale expression) to a
  frame's bounds.
- **Animations.** Every frame of a model is an animation: Quake MDL/MD2/MD3 frames with their names, and one
  frame per assimp animation (the animation's first frame), named after the animation — a studio model's
  sequence such as `sitting2` (§15). The property that selects the frame is found from the class's model
  definition, not hard-coded: the model expression is evaluated with a recording `el::VariableStore` wrapper,
  and the first variable read whose value n selects frame n (tried for several n) without changing the model
  path is the frame property — `sequence` for the Half-Life FGD, `frame` for `"frame": frame`, none for a fixed
  frame. `entity_model_info` reports `frameProperty`, `currentAnimation`, `animationCount` and `animations`
  (`{index, name, bounds, worldBounds}`, at most `maxAnimations`, default 100), and `modelLoaded` /
  `modelBounds` also for models it loaded itself. `entity_animation_set` sets the frame property by animation
  name (case-insensitive) or index in one undo step and returns the resulting model bounds.
- **Placement checks.** For a point entity with a loadable model, five vertical rays go down from the top of the
  current animation's world model box (center and inset corners) against solid and brush-entity brushes and
  patches, hidden ones included because the compiler builds them (not triggers and not layers omitted from
  export). A hit from 1 unit above to 2 units below the model bottom supports it (e.g. the floor under a chair
  seat); otherwise the highest first hit is the surface: `MODEL_BELOW_FLOOR` (more than 2 units deep:
  animation bounds enclose every vertex, feet routinely dip a unit or two, and the games drop monsters by
  their hull; depth and `suggestedMove`), `MODEL_FLOATING` (more than 1 unit gap), `MODEL_NO_FLOOR`.
  `placementRule` decides which entities get these checks, for `issues_list`, `issuesIntroduced`, the tool
  warnings and `map_check` alike: only standing classes (below) and no sprite models; entities that may float
  (flying or swimming monsters, origin in a liquid) get no `MODEL_FLOATING` / `MODEL_NO_FLOOR`. Other brushes that intersect the
  model box shrunk by 1 unit (`intersectsInterior`) give `MODEL_PENETRATES_BRUSHES`. `entity_create_point`
  and `objects_move` add them as warnings (at most 20, then `MORE_PLACEMENT_FINDINGS`);
  `entity_placement_check` lists them for ids, the selection or the whole map (`scope: "map"`,
  `onlyProblems`). `map_check` (E13) is to reuse `checkModelPlacement`. `entity_create_point` takes
  `dropUsing: "auto" | "model" | "definition"`; `auto` rests the current animation's model bottom on the floor
  when the model loads and uses the definition bounds otherwise.
- `entity_move_brushes` to `world` is the editor's Make Structural (smart tags such as detail are turned off).
- `entity_links_get` without ids lists the whole map. `entity_link` reuses the target's name or generates
  `<classname>_<n>`, unique among all link values. `entity_color_set` converts to the property's color range
  from the definition and defaults to the class's first color property, else `_color`.

### 10.8 Materials, faces and smart tags

- **Material names** are compared case-insensitively, like `MaterialManager`; a loaded material's own spelling
  is used when it is applied. `materials_list` computes usage from the map's brush faces and patches
  (`gl::Material::usageCount` also counts faces held by undo snapshots), filters by substring or `*`/`?` glob,
  and lists used but unloaded materials with `includeMissing` (`missing: true`, without `__TB_empty`).
- `material_apply` and `face_attributes_set` warn with `UNKNOWN_MATERIAL` and apply the name anyway, as the
  editor does. `material_set_current` is `Mutation::External` (`Map::setCurrentMaterialName`).
- **`material_replace`** takes `from`/`to` or `rules[]`; each `*`/`?` in `from` captures text that fills the
  corresponding wildcard of `to` (`wall_old*` → `wall_new*`), `*` is greedy, and the first matching rule wins.
  The scope is one of `scope` (`selection` | `map`), `layer` (id or name) or `ids`; the default is the
  selection, or the map if nothing is selected (the editor's Replace Material dialog). Only the material name
  changes (`setBrushFaceAttributes` with `materialName`), so the alignment is kept. Hidden and locked faces
  are skipped and counted (`skippedFaces`). Targets that are not loaded are reported in `unmatched` and left
  unchanged unless `allowMissingTargets` is set; patterns that match nothing are listed in `noMatch`. The
  result counts faces per `{from, to}` pair. Brush faces only.
- **`material_preview`** reads mip 0 from the texture's CPU buffers. The editor drops them after the GL
  upload, so otherwise it rebuilds the game file system like `Map::loadMaterials` (game path, mods, WAD
  property, palette) and calls `mdl::loadTexture`. RGB/BGR/RGBA/BGRA are box-filtered to at most `maxSize`
  and written as PNG with miniz (`CallContext::addImage`); compressed formats fail with `UNSUPPORTED`. No GL
  context is needed.
- **Rotation of Valve faces.** Parallel (Valve 220) UV coordinate systems keep explicit UV axes, and their
  stored rotation is bookkeeping: `ParallelUvCoordSystem::computeRotationAngle` measures the transformed U axis
  against the V axis, so every transform with alignment lock, even a pure translation, adds ±90° to the stored
  rotation while the axes (and the texture) stay correct. `faceRotation` (`NodeJson`) therefore derives the
  rotation of Valve faces from the UV axes: the angle about the UV normal from the initial axes of a new face
  with the same normal (replicating `ParallelUvCoordSystem`'s `computeInitialAxes`) to the projected U axis.
  All face descriptions report it, and `face_attributes_set` turns an absolute `rotation` of Valve faces into a
  per-face `AddValue` of the difference, so setting the reported value leaves the texture unchanged. Standard
  faces report and set the stored rotation.
- **Face attributes.** `face_attributes_get` (paginated) reports offset, scale, rotation, the UV axes, flags as
  `{bits, names, unknownBits, fromMaterial}`, value and color, plus a `format` block (map format, Valve or
  Standard UVs, whether the format saves surface attributes and colors, the game's flag names). Flag names
  are computed by value, since `FlagsConfig::flagNames` assumes consecutive bits. `face_attributes_set` maps
  absolute values to `SetValue`, `offsetBy`/`rotateBy` to `AddValue`, `scaleBy` to `MultiplyValue`, flags
  `{set}` / `{add, remove}` (names or bits) to `SetFlags` / `SetFlagBits` / `ClearFlagBits`, and `unset` to
  the material defaults. Warnings: `UNKNOWN_FLAG` (the name is skipped), `ATTRIBUTE_NOT_SAVED` (the format
  does not store flags, value or color).
- `face_attributes_copy` mirrors the editor's alt-click: `project` (`copyUv` with `WrapStyle::Projection`),
  `rotate` (`WrapStyle::Rotation`; `ROTATION_NEEDS_VALVE_FORMAT` in Standard format) and `material`; the
  source face is never a target.
- **`uv_align`** has one `operation`: `justify` (left/right/up/down/center), `align`, `fit` (fits the face,
  then scales so the texture repeats `repeatU` × `repeatV` times, justified to the face edge; `trimSheet`
  uses the editor's trim-sheet fit), `autoFit`, `reset` (`resetAll` with the game's default UV attributes),
  `resetToWorld` (`resetAllToParaxial`), `flip` (scale × -1) and `rotate90` (rotation ± 90), the latter four
  as in `UvEditor`, and `typical` (below). `policy` is the editor's best/next/prev. Arguments of other operations are
  `IGNORED_ARGUMENT` warnings; fit with repeats needs a loaded material (`MATERIAL_NOT_LOADED`).
- `uv_nudge` changes offsets in each face's own texture axes (not camera-relative like `translateUv`),
  accounts for negative scales, and rotates by the grid angle by default.
- **Smart tags.** `SmartTag` does not expose its matcher, so `tags_list` classifies it from its printed form
  (classname, material, surfaceparm, content or surface flags) and tells content from surface flag matchers
  apart by testing a face; flag masks of names the game does not define are reported as `invalidflags`.
  `tag_apply` / `tag_remove` select exactly the targets (`withTargets` for object tags, where brush entity ids
  stand for their brushes as in `resolveTargets`; `withFaces` for face tags) and call `SmartTag::enable` / `disable` with an MCP
  `TagMatcherCallback` that picks `option` or the first choice with a `TAG_OPTION_CHOSEN` warning. Material
  tags cannot be removed (`UNSUPPORTED`, hint: `material_apply`). Results list the targets that carry the tag
  afterwards; created brush entities also appear in the change report.
- **Material knowledge (E11).** A `MaterialKnowledge` (one per call, `materialKnowledge(CallContext&)`) merges
  knowledge notes, the game's smart tags, a scanned reference corpus, statistics of the current map, a name
  fallback and a CPU image analysis into `MaterialProfile`s: kind (`panel`, `tile`, `trim`, `decal`, `sky`,
  `liquid`, `tool`, `unknown`), texture size, typical scale per axis, scale range (min, 10th and 90th
  percentile, max), texel density (world units per texel), typical face size, typical repeats, whole-repeat and
  aligned fractions, and the image analysis. Each value carries its `source` (`notes`, `config`, `corpus`,
  `map`, `name`, `image`) and sample count. Precedence: notes > corpus > current map > image; for the kind,
  notes > config (smart tags) > corpus > map > name > image, because the image cannot recognize sky or liquid.
  Nothing is specific to a game: config kinds come from face tags that match a probe face with the material
  (names containing sky → sky; liquid, water, lava, slime → liquid; clip, skip, hint, origin, null, nodraw,
  caulk, trigger, ... or a `transparent` tag → tool); the name fallback only knows sky and liquid prefixes.
- **Face sampling.** `sampleFace` measures a face as the corpus, the map statistics and the UV checks see it:
  |scale| and flips per axis, the rotation (`faceRotation`, so Valve faces use their UV axes), the extent
  along the texture axes in world units and texels, the repeats, where the texture starts relative to the
  face's min edge, whether a texture edge lies on a face edge (within 1 texel) and whether the repeats are
  whole. Statistics are capped histograms (16 entries, dropped entries merged into the nearest kept one, exact
  min and max kept), so a corpus of a million faces stays a few MB. With at least 4 sized samples, whole
  repeats in ≥ 75% with median repeats ≤ 2 and ≥ 75% aligned is a panel; one axis fitted (≤ 1.05 repeats,
  aligned) in ≥ 75% while the other is not in ≥ 50% is a trim; whole repeats in < 60% or median repeats > 2
  is a tile.
- **Image analysis.** `analyzeImage` compares opposite edges (left/right columns, top/bottom rows) with the
  mean difference between adjacent columns or rows inside the image: an axis tiles when the edge difference is
  at most 1.5 × that + 0.03, so noisy tiles still count as seamless. Both axes tile → tile; one axis or an
  aspect ratio of at least 4:1 → trim; ≥ 25% pixels with alpha < 128 → decal; otherwise panel.
- **Knowledge store.** `McpHost::knowledgeDirectory()/<game>/<mod or _game>/` holds `corpus.json` and
  `notes.json` (sanitized folder names; the mod is the document's most specific enabled mod). Mod notes replace
  the game-level note of the same material; the mod's corpus is consulted before the game's. Parsed files are
  cached by path, last write time and size; files are written through a temporary file and a rename. An invalid
  file is ignored with a `KNOWLEDGE_FILE_INVALID` warning. Without a knowledge directory the scan and notes
  tools fail with `UNSUPPORTED_IN_HOST`; `material_usage` still works.
- **`material_corpus_scan`** is an asynchronous `Mutation::External` tool: it lists the matching files
  (`pattern`, `recursive`), reads one file per deferred step with progress (cancel writes nothing), each in its
  own format (header, else the game's formats, then all), samples every brush face and discards the brushes,
  and replaces or merges (`mode`) the corpus of the document's game and mod. Texture sizes come from the
  document's loaded materials, so the game's WADs should be loaded first; unparsable files are listed, not
  fatal. `material_notes_set` (`scope: game | mod`) merges per-material facts (kind, scale, faceSize, text),
  clears fields or removes notes; `material_notes_get` pages them. `material_usage` returns profiles for names
  or globs (an exact name wins over a glob, since Quake liquids start with `*`), by default for the materials
  of the selection or the 20 most used in the map.
- **Texturing checks.** `checkUv(faces, map, ProfileProvider, only)` (`UvCheck`) finds, per face:
  `UV_ASPECT_DISTORTION` (the texel density ratio U/V deviates by more than 10% from the expected ratio: the
  typical scale's ratio from notes or corpus, else 1:1); `UV_FRACTIONAL_REPEAT` (panels on both axes, trims
  across their short axis — for a square texture the axis the image says does not tile — repeats not whole
  within 1 texel, less than one repeat included); `UV_PANEL_NOT_ALIGNED` (no texture edge on a face edge);
  `UV_UNUSUAL_SCALE` (|scale| outside the 10th–90th percentile range by more than a factor 1.25, with a range
  from notes, the corpus (≥ 5 samples) or the map (≥ 20 samples)); `UV_TEXEL_DENSITY_MISMATCH` (tiles: the mean
  density differs by more than 1.5× from a neighbour — faces of the same brush sharing an edge, or coplanar faces
  of other brushes with overlapping edges, found through the world node tree; reported on the face further from
  its typical scale); `UV_SEAM` (coplanar touching neighbours with the same tile, trim or unknown material whose
  UV mapping does not continue across the edge by more than 1 texel). Tool, sky and liquid materials are
  skipped, size-dependent checks need a loaded material, and a kind from fewer than 20 map samples gives way to
  the image analysis (the checked faces themselves shape the map statistics). Each finding has the measured
  values, a concrete fix (tool and arguments) and alternatives. A whole map of 10,000 faces takes about 1 s in
  a Debug build.
- **`uv_check`** (read-only, paginated) checks faces, brushes, groups or entities (`ids`), the selection or the
  map (`scope`), filtered by `codes`, with counts per code. `material_apply`, `material_replace`,
  `face_attributes_set` and `uv_align` report the findings on the faces they changed as warnings with the
  finding code (`warnUvFindings`; at most 10, then `UV_CHECK_MORE`). `map_check` (E13) is to reuse `checkUv`.
- **Fitting to the material.** `uv_align` `fit` takes `keepAspect` (with only `repeatU` or `repeatV`, default
  `repeatU` 1: the other axis follows the expected ratio) and `round` (whole repeats, at least 1). The
  operation `typical` applies the material's typical scale (notes, corpus, then the map if other faces use the
  material, else the game's default with `TYPICAL_SCALE_DEFAULT`), keeps the signs and justifies the texture
  like `fit`. Both report `fits` (scale, repeats, typical scale with its source). `material_fit_geometry`
  (read-only) turns it around for panels: for a face and a material it returns the panel size at the typical
  scale, the face size along the texture axes, the smaller and larger sizes with whole repeats and, per axis,
  the resize (`delta`) with a `face_extrude` call on the adjacent face at the edge the texture axis points to,
  then `uv_align typical`. For tiles it warns `MATERIAL_IS_TILE`; for trims only the axis across the strip is
  relevant.

### 10.9 Actions (E14)

`ActionTools.cpp`: `actions_list`, `action_invoke`. `ActionHost` (`ui::McpActionHost`, `FakeActionHost`) lists the
main menu (`ActionManager::visitMainMenu`, with the menu path), the map view actions (`visitMapViewActions`, sorted by
path) and the document's tag and entity definition actions (`createTagActions` / `createEntityDefinitionActions`,
created on every call because they refer to the document's tags and definitions). The path is the action's generic
preference path; shortcuts are the `KeySequence` values of `pref(action.preference())`; the context is
`actionContextName`; enabled and checked are evaluated with `ActionExecutionContext{appController, window, view}` for
the requested view ("3d", "xy", "xz", "yz"; default `currentMapViewBase()`), whose action context `Action::enabled`
checks. The host additionally reports `Entities/<brush class>/Create` as disabled without selected brushes or patches
(`mdl::createBrushEntity` requires them). `invokeAction` runs the action like the menu or the view's shortcut; deferred
actions run from a zero timer owned by the window and look the action and the view up again by path.

`ActionCatalog` (public, Qt-free) classifies every action path as Invoke, Dialog (kind modal, file, input,
confirmation, menu, window or browser) or Refuse, with the semantic tools that do the same, plus patterns for
`Filters/Tags/<t>/Toggle Visible`, `Tags/<t>/Enable|Disable` and `Entities/<c>/Toggle|Create`. Unknown actions are
Dialog if their label ends with "...", else Invoke. Refused: Undo, Redo and Repeat (they would act inside the call's
transaction), Reload Material Collections / Entity Definitions (use the reload tools), the four entity link view
filters (the editor stores the link mode in the face render mode preference; `view_options_set` sets it correctly),
Debug Crash and Throw Exception. Dialog actions include New, Open, Save, Save as, Export, Load Point / Portal File,
Revert, Close, Preferences, About, Move objects, Replace Material, Select by Line Number, Group, Rename Groups, Move
Camera to, Compile, Launch, Rerun, Manual and Tag Enable (a popup menu when a tag allows several values).

`actions_list` (paginated) filters by `kind`, `menu` (path prefix), `query`, `handling`, `enabledOnly` and `view`;
items have `path, label, kind, menu, shortcuts, enabled, checked, opensDialog, invokable, tools` (`detail: "full"`
adds `context, checkable, handling, dialog, reason`). `action_invoke` takes a `path` (or a unique label), `view` and
`openDialog`; it is `Mutation::Map` with `ToolDef::keepsActiveTool()`: the call runner does not run
`prepareForAgentEdit`, so the action runs in the editor's current state (tool actions such as Perform Clip work), and
map changes become one undo step "AI: Invoke Action" with a change report; tool, view filter, grid and camera actions
leave the transaction empty (no undo step). A dry run checks the action without running it. Dialog actions fail with
`DIALOG_REQUIRED` naming the semantic tools unless `openDialog`, which runs them deferred after the call (outside the
transaction; later modifying calls wait while the modal dialog is open); refused ones fail with `ACTION_REFUSED`.

**Coverage check (E14.8).** `tst_McpActionHost.cpp` enumerates the whole registry for a real Quake document (main
menu, map view, tag and entity definition actions), checks that `McpActionHost` lists every action once and that the
catalog classifies it, that every catalog path still exists (upstream renames fail the test) and that every tool the
catalog names is registered, and requires at least 95% of the actions to be reachable through a semantic tool or
`action_invoke`. In a Debug build 390 of 395 actions (98.7%) are reachable; the other five are debug-only (Crash,
Throw Exception During Command, Show Palette, Set Window Size, Show Crash Report Dialog), which release builds do not
have.

### 10.10 Compiling (E7)

- **Execution.** Compilations run in the editor process through `CompileHost`. `ui::McpCompileHost`
  reuses the editor's `CompilationRun` / `CompilationRunner` unchanged: each job owns a hidden `QTextEdit`
  that receives the runner's output (its plain text is the log), a copy of the window's perspective camera
  and the `CompilationRun`. Tools run as `QProcess`es, so neither the UI nor other MCP calls wait. A test
  run ends inside `startCompile`. Reloading the document cancels the job and logs
  `#### Terminated: the document was reloaded` (the runner refers to the document's `Map`).
- **Runs** (`CompileRuns`, owned by `ServerState`): `run:<n>` handles numbered per server; each run keeps
  the profile, its enabled tasks, the preset name, game, document handle, session, test flag, times, the job
  and a log snapshot. All running runs and the 20 most recent ended runs are kept (pruned when a run
  starts). Callbacks look runs up by number through a weak alive flag, so `ended` may fire inside
  `startCompile` or `cancel()`. Closing a document cancels its running runs, snapshots their logs and
  destroys the jobs; destroying the registry destroys all jobs without callbacks. Output and the end of a
  run schedule an update of the log resource; the end also updates the editor status.
- **One compile per document.** `compile_run` fails with `COMPILE_RUNNING` (naming the run) while an MCP run
  or the compilation dialog compiles the document; `document_close` and `document_revert` refuse while an MCP
  run is active (`COMPILE_RUNNING`, hint `compile_cancel`). `editor_status.compileRunning` covers both.
- **`compile_run`** takes a saved `profile` or a `preset` (exactly one) and `test`. The map must have been
  saved once (`UNSAVED_CHANGES`, hint `document_save_as`); unsaved changes are compiled because the export
  task writes the current state. Unless `test`, every game tool variable (`${qbsp}`) used by an enabled run
  tool task must point to an executable file (`OPERATION_FAILED` with `details.tools`, hint
  `compile_tools_set`); an invalid game path is a `GAME_PATH_NOT_SET` warning, and relative entries in the
  map's WAD list are a `RELATIVE_WAD_PATH` warning (compile tools open them relative to their working
  directory). It returns the `compile_status` payload of the new run.
- **Presets** (`CompileUtils`): the family is derived from the game's compilation tool names — csg/bsp/vis/rad
  (Half-Life, VHLT/ZHLT), qbsp/vis/light (Quake, ericw-tools), bsp/vis/light with a Quake 2 map format (Quake
  2, ericw-tools 2 with `-q2bsp`), q3map2 with the search path `baseq3` (Quake 3). Every preset uses
  `${MAP_DIR_PATH}` as working directory, exports to `compile/${MAP_BASE_NAME}.map` (TrenchBroom properties
  stripped), runs the tools with quoted paths and `treatNonZeroResultCodeAsError`, and copies
  `compile/${MAP_BASE_NAME}.bsp` (Quake: also `.lit`) to `${GAME_DIR_PATH}/${MODS[-1]}/maps`.

  | Family | fast | normal | full |
  |---|---|---|---|
  | Half-Life | csg, bsp, rad `-fast` | csg, bsp, vis, rad | csg, bsp, vis `-full`, rad `-extra` |
  | Quake / Quake 2 | qbsp, light | qbsp, vis, light `-extra` | qbsp, vis `-level 4`, light `-extra4 -bounce` |
  | Quake 3 | `-meta`, `-light -fast` | `-meta`, `-vis -saveprt`, `-light -fast -filter` | `-meta`, `-vis -saveprt`, `-light -fast -super 2 -filter -bounce 8` |

  q3map2 stages get `-game quake3 -fs_basepath "${GAME_DIR_PATH}" -fs_game ${MODS[-1]}`. Presets can be run
  directly or saved as editor profiles with `compile_profile_save {"preset": ...}`.
- **Profiles**: `compile_profiles_list`, `compile_profile_save` (`overwrite` for an existing name, else
  `FILE_EXISTS`) and `compile_profile_delete` edit the game's `CompilationConfig` through
  `GameManager::updateCompilationConfig`, the same store the compilation dialog uses. Tasks are JSON objects
  with `type` (`exportMap`, `copyFiles`, `renameFile`, `deleteFiles`, `runTool`, `launchEngine`) and the
  type's keys; keys of other types are `INVALID_ARGUMENT`.
- **Tool paths**: `compile_tools_get/set` read and write each tool's path preference
  (`Games/<game>/Tool Path/<tool>`); a path that is missing, not a file or not executable is still set, with
  a `TOOL_NOT_FOUND` / `TOOL_NOT_A_FILE` / `TOOL_NOT_EXECUTABLE` warning.
- **Status** (`CompileLog`): the runner reports no structured events, so `compile_status` analyzes the log.
  The runner's `#### ...` lines split it into the enabled tasks (the k-th start or failure line belongs to the
  k-th task): state per task, exit codes, the current task, `completedTasks`, and the executed commands,
  exported maps and copied files. Tool messages in the formats of VHLT/ZHLT, ericw-tools/tyrutils, q3map2 and
  Quake 2 tools become errors and warnings (runner failure lines count as errors). Leaks are recognized from
  `=== LEAK in hull 0 ===` / `Entity <class> @ (x, y, z)` or `Entity <class> at (x y z)`, `Reached occupant
  ... at (x y z)` / `reached occupant at: (x, y, z)`, `Leak file
  written to ...` and the `leaked` banners. The run state is `cancelled` (cancel requested, document closed,
  or `#### Terminated`), `failed` (a task failed or not all tasks completed) or `succeeded`. The compiled file
  is the first copied `.bsp` source (else the exported map's `.bsp` if it exists); `copiedTo` lists the
  copies; the point file is the reported leak file, else `<exported map>.pts` or `.lin`.
- **Point and portal files**: `pointfile_load` defaults to the latest run's leak file, then
  `compile/<base>.pts`, `<base>.pts` and the `.lin` variants; it returns the path (at most 1000 points), its
  length, the three point entities nearest to each end, and `leavesMapAt`, where the path, walked from the end
  inside the brushes' bounds, leaves them; if both ends are inside them (the leak ends in the void between
  rooms), the first point, sampled every quarter cell from the end that is not in the void, that the space
  analysis (`analyzeSpaces`) labels as void; else null. `portalfile_load` defaults to `compile/<base>.prt`, then
  `<base>.prt`. Both use `MapDocument::loadPointFile` / `loadPortalFile`, so the editor shows them.
- **Engines** (`EngineTools.cpp`): `engine_profiles_list` and `engine_profile_save` read and write the game's
  `GameEngineConfig` through `GameManager::updateGameEngineConfig`, the store of the editor's engine dialog.
  - Each profile is reported as `{id, name, path, parameters, status, exists, executable}` (`status`: ok, notSet,
    notFound, notAFile, notExecutable; a macOS `.app` folder is ok); the id is what `launchEngine` compile tasks
    refer to. The list warns `ENGINE_CONFIG_INVALID` if the configuration could not be parsed.
  - `engine_profile_save` requires an absolute path. An existing name needs `overwrite` (else `FILE_EXISTS`);
    overwriting keeps the id, and keeps the parameters when they are omitted; a new profile gets a new UUID. A
    missing or non-executable engine is saved with a `TOOL_NOT_FOUND` / `TOOL_NOT_A_FILE` / `TOOL_NOT_EXECUTABLE`
    warning.
  - `engine_launch` starts a profile (by name or id; optional when the game has exactly one) through `EngineHost`,
    detached, without confirmation, and returns `{profile, path, parameters (interpolated), processId}` without
    waiting. `parameters` overrides the profile's spec for one launch. A never-saved map is `UNSAVED_CHANGES` (hint
    `document_save_as`); unsaved changes are an `UNSAVED_CHANGES` warning, because the engine loads the last
    compiled `.bsp`. An unknown or ambiguous profile, a game without profiles and a spec that cannot be
    interpolated are `INVALID_ARGUMENT` (as in `compile_run`); a missing engine and a failed start are
    `OPERATION_FAILED`. The dry run returns the interpolated parameters and `wouldDo`.

### 10.11 Layers, groups, clipboard and import (E9)

**Layers and visibility (`LayerTools.cpp`).** The tools follow `ui::LayerEditor` and use only the `mdl`
layer, visibility, locking and node functions. Positions count from 0, which is always the default layer.
`layers_list` reports every layer in list order with `id`, `name`, `default`, `position`, `sortIndex`,
`current`, `hidden`, `locked`, `omitFromExport`, `color` and object counts by kind. `layer_create` adds the
layer at the bottom like the editor, then moves it to `position` / `after` with `moveLayer`, and makes it
current unless `makeCurrent: false`; a duplicate name warns `DUPLICATE_LAYER_NAME`. `layer_remove`
deselects everything, moves the layer's children to the default layer, makes the default layer current if
needed and removes the layer; like the editor it needs another visible, unlocked layer and refuses the
default layer (as do `layer_rename` and `layer_reorder`). `layer_reorder` takes exactly one of `position`
and `offset`. `layer_set_state` applies isolate, hidden, locked, omitFromExport and then current; hiding uses
`hideNodes`, showing and unlocking reset the state to inherited, `current` can only be set to true, and a
hidden or locked current layer warns `CURRENT_LAYER_HIDDEN` / `CURRENT_LAYER_LOCKED`.
`objects_move_to_layer` refuses objects inside groups (the hint names the group), moves a brush entity
when one of its brushes is given, leaves the moved objects selected like the editor, and warns
`LAYER_HIDDEN` / `LAYER_LOCKED` for such a target and `NO_CHANGE` when nothing moves. `visibility_set`:
`hide` also accepts locked objects and objects in closed groups; `show` needs ids, resets hidden objects to
inherited and forces the rest visible, even inside hidden layers or groups; `isolate` leaves its targets
selected; `show_all` resets object visibility only and lists still hidden layers in a `HIDDEN_LAYERS`
warning. Results report `hiddenObjects`.

**Groups and linked groups (`GroupTools.cpp`).** The tools wrap `Map_Groups` and `reparentNodes`. Creating,
ungrouping, merging, adding and duplicating leave the result selected; renaming and separating restore the
selection. `group_create` returns `{group, objects}`; `group_ungroup` unlinks a linked group first and the
children keep their ids; `groups_merge` moves the other groups' objects into `target` and removes the
emptied groups. `group_add_objects` / `group_remove_objects` replace brush-entity brushes by their entity
like the editor; adding skips objects already in the group (all skipped is `INVALID_ARGUMENT`), and linked
copies receive clones. Removal moves objects to the group's parent (enclosing group or layer), removes
emptied groups and closes open groups that no longer contain the objects. `group_remove_objects` and
`linked_group_extract` accept objects in closed groups if they are visible and unlocked, and open or close
groups as needed. `group_open` opens a group like a double-click (enclosing groups opened, others closed);
`group_close` closes the current group or, with `all`, every open group (`NO_OPEN_GROUP` warning when none
is open); both return `{openGroup, openGroups}`. `linked_group_duplicate` creates `count` (1..64) linked
copies, copy *i* moved by *i*·`offset` (count > 1 needs an offset). `linked_group_select` selects the link
sets of the given groups or of the linked groups containing the given objects, skipping unselectable members
with a `NOT_SELECTABLE` warning. `linked_group_separate` unlinks the given groups (given groups of one set
stay linked to each other; passing a whole set is an error). `linked_group_extract` calls
`mdl::extractLinkedGroups` and returns the source's new group, the extracted objects and source→extracted
pairs, then restores the previously open groups. Linked-group propagation runs when the call's transaction
commits; ids in the other copies survive through `IdRegistry` aliasing (§5.2) and are reported as `modified`.

**Clipboard and import (`ClipboardTools.cpp`).** The server keeps one clipboard, `ServerState::clipboard`,
shared by all sessions and documents; the operating system's clipboard is never touched (TbMcpLib has no
Qt). `clipboard_copy` (read-only) writes the objects (`ids` or the selection) or the faces (`faces` or the
selected faces) with `mdl::NodeWriter` into the clipboard and returns the text (`includeText`);
`clipboard_cut` copies and deletes in one undo step (a dry run leaves the clipboard unchanged).
`clipboard_paste` pastes `text` or the clipboard with `mdl::paste`, which accepts the document's format or a
compatible one. Text whose first significant character is `(` is face text: the last face's attributes are
applied to the target faces through `withFaces`. Pasted objects are moved with `translateSelection`: with
`position`, the chosen `anchor` of their bounds (`min` by default, `center`, `max`, `bottomCenter`) lands on
the point; with `offset`, they move by the vector; `snapToGrid` then rounds the min corner to the grid;
`targetLayer` moves them with `moveSelectedNodesToLayer`. They stay selected, and materials that are not
loaded are reported. `map_file_inspect` (read-only) lists another file's format, layers (by index), groups,
classnames, materials and the materials missing from the current document, so that an agent can choose what
to import. `map_import` reads the file with `ImportReader`, a `mdl::MapReader` subclass that parses in the
source format and builds faces in the document's format (the Standard/Valve UV conversion `mdl::paste` uses,
also for incompatible pairs such as Quake 2 → Standard). The source format comes from the header comment;
without one the reader tries the formats compatible with the document's format, then the game's formats,
then all formats. Objects are filtered by `layer`, `group`, `classname` and `region` / `regionMode`; patches
are dropped when the target format has none (`PATCHES_DROPPED`). The kept objects are written in the
document's format and pasted through the `clipboard_paste` path with the same placement options, into
`targetLayer` (default: the current layer; source layers are flattened). The result lists the new ids, the
missing materials, and selects the imported objects; the call is one undo step, `AI: Import Map`.

### 10.12 Agent vision (E10)

Agents render images of the map from their own cameras. Nothing the user sees changes: no camera, view
filter, hidden state, selection or preference of the editor is touched, no window opens, and the calls do not
wait for the user.

**Split.** The core (`SnapshotTools.cpp`, Qt-free) resolves cameras and visibility options into a
`SnapshotRequest` (`Snapshot.h`): an `AgentCamera`, the image size, `SnapshotOptions` (face mode, shading,
fog, edges, entity models, bounds, classnames, entity links, leak path, grid, axes, background) and a
`SnapshotScene` (the nodes to draw, a face filter, highlighted nodes and color, markers). The host's
`SnapshotRenderer` draws exactly that and returns an `RgbaImage`; the core encodes PNG (miniz) or asks the
renderer for JPEG. Tests use `FakeSnapshotRenderer` (`FakeHost::snapshot`, `snapshotRendererOverride`,
`supportsSnapshots`), which records every request and returns an image that changes with the drawn objects.

**Agent cameras** (`AgentCamera.h`): perspective (position, direction, up, fov as the editor uses it) or
orthographic (direction, up, zoom = pixels per unit; top/front/side as in `MapView2D`). Helpers: direction
from yaw/pitch (yaw counterclockwise from +X, pitch up positive) and back, look-at, orthographic views placed
outside the bounds, `frame` (fit a box for the direction, fov and image aspect with a margin; orthographic:
zoom to fit), `orbit` (target, yaw, pitch, distance), `eyeHeight` (a ray down from a point inside a room to
the floor, then the game's eye height: Quake and Quake 2 46, Half-Life 64, Quake 3 50, other games 48; the game
is matched by name, then by its compile tools). Framing without an image size assumes 1024×768. Named
cameras live in the `Session` (at most 64) and are never shown as the user's camera.

**Tools** (all `Mutation::None` with asynchronous handlers, §4.2; no undo steps):

- `agent_camera_set/get/list/delete`: `camera` is a perspective form (`position` + `lookAt` | `direction` |
  `yaw`/`pitch`, `fov`), an orthographic form (`view` top/front/side or xy/xz/yz, `center`, `zoom`) or a
  helper (`frame {ids | box, margin}`, `orbit {target | ids, yaw, pitch, distance}`, `eyeHeight {point,
  height}`).
- `view_snapshot`: `camera` (a name or an inline camera; default: frame everything drawn from yaw 45, pitch
  −30), `width`/`height` (default 1024×768, 16–2048), `format` png/jpeg (+`quality`; JPEG falls back to PNG
  with a warning when the renderer cannot encode it), `saveTo` (+`overwrite`), `keepAs`, `options`
  {faceMode, shading, fog, edges, entityModels, bounds, classnames, entityLinks, leakPath, grid, gridSize,
  axes, background, hideTags, hideClassnames, brushes, pointEntities, brushEntities, patches, includeHidden},
  `isolate`, `highlight {ids, color}`. Returns the image metadata, the camera, counts of drawn objects and
  `resourcesPending`; one image content block.
- `view_snapshots_around`: `ids` or `box`, `views` (north/east/south/west, the diagonals, `above`,
  top/front/side, or `{label, camera}`; at most 12); a text label block before each image; progress per image.
- `map_plan_view` `format: "image" | "both"` (default `"text"`): an orthographic camera at the slice height
  looking down (near 0, far = `floorDepth`), up to 16 px per cell and at most 1024 px (`imageWidth`,
  `imageHeight`), every point entity of the region as a marker in the legend's colors at its origin, with z
  clamped into the camera's depth range so that entities above the slice (lights) are marked too.
  `entitiesTruncated` is always present and true only when `maxEntities` dropped entities from the list. It stays
  synchronous.
- `view_snapshot_compare`: `before` (a kept snapshot) and optional `after` (default: render now with the
  before snapshot's camera and options), or `undoSteps` (render, undo n steps, render, redo n steps, all in one
  step so no event loop runs in between; refused while any transaction is open, the user is busy, or fewer
  steps exist; the result's `historyRestored` checks the modification count and both stacks), plus
  `threshold`. Returns before | after side by side and a changed-pixel mask, with the changed pixel count,
  ratio and bounds. Kept snapshots live in the `Session` (at most 8; the oldest is dropped with
  `SNAPSHOT_DROPPED`).
- `view_snapshot_user`: `view` 3d/xy/xz/yz or `listOnly`; returns the user's view image and its camera.
- `view_pick` (`PickTools.cpp`, synchronous): `snapshot` (a `snapshotId` or a `keepAs` name), `pixel {x, y}` or
  `pixels` (at most 256), `includeHidden`, `ignoreTriggers`, `ignorePointEntities`, `kinds`, `ignore`,
  `maxDistance`. Returns `{snapshot, width, height, camera, hit (single pixel), picks: [{pixel, ray, hit | null}]}`;
  a hit has `object`, `kind`, `label`, `face`, `faceIndex`, `material`, `normal`, `point`, `distance`, `depth`,
  `entity`, `classname`, `group`, `layer` and `bounds` (`rayHitJson` in `SpatialTools.h`, shared with `ray_pick`
  and `space_check`; point entities get the normal of their bounds face). Unknown or dropped snapshots, pixels
  outside the image and snapshots of another document are `INVALID_ARGUMENT` (the hint lists recent ids).

**Snapshot ids and picking.** Every rendered image (`view_snapshot`, each image of `view_snapshots_around`, the
`map_plan_view` image, the "after" image of `view_snapshot_compare`, `view_snapshot_user`) returns a `snapshotId`
(`snap:<n>`, per session). The `Session` keeps camera, image size, document and the view arguments of the last 32
snapshots (no pixels). `view_pick` rebuilds the scene of the snapshot from its stored arguments (editor-hidden
objects, `hideTags`, `hideClassnames`, `isolate`, face tags) and casts the ray of each pixel through the pixel
center (pixel (0, 0) is the top-left corner) against the **current** map, skipping hits outside the near and far
planes. `CameraProjection.h` holds `makeGlCamera` (used by the renderer too, so pick rays and images agree) and
`ImageProjection` (pixel → ray, point → pixel and depth in double precision, including the orthographic viewport
rounding); orthographic cameras may have a near plane of 0 (`map_plan_view`).

**Annotations** (`Annotations.h`). `view_snapshot` and `view_snapshots_around` take `annotations {labels: true |
{ids, max ≤ 100 (30)}, grid: true | {step (64), planes floor|walls|both, box, labelEvery}, compass: true, player:
{point, onFloor}}`. The core draws them onto the rendered `RgbaImage` (so they work with every renderer, including
the fake one) with a built-in 5×7 font scaled with the image size. Labels (id, classname or group name, size) sit at
the projected bounds center or the bounds face turned most towards the camera and are skipped when occluded or
overlapping (greedy). The grid without a `box` covers the floor, ceiling and walls found by rays around the surface
at the image center; lines behind geometry are hidden by visibility rays (a budget, about every 8 pixels). The
compass points along the camera's horizontal forward direction (+Y north); the player is the game's player box
(`playerSize`) with an eye-height ring standing on the floor below the point. The result reports what was drawn
(`annotations {labels, labelled, labelsSkipped, grid, compass, player}`; warnings `GRID_STEP_INCREASED`,
`GRID_NOT_DRAWN`, `NO_FLOOR`). A `keepAs` snapshot keeps the image without annotations so that comparisons stay
clean.

**Visibility.** Objects hidden in the editor stay hidden unless `includeHidden`; `isolate`d ids are always
drawn. The editor's view filters (hidden tags, entity classes, show flags of the map's `EditorContext`) are
not applied — each snapshot has its own. Tag names are case-insensitive; object tags remove objects, face
tags go into the face filter (a brush without visible faces is removed); unknown tags warn `UNKNOWN_TAG`.
`hideClassnames` takes globs; `worldspawn` hides world brushes.

**Slices.** Each image is rendered in its own deferred step with the scene rebuilt from ids (no node pointer
is kept across steps); progress per image, cancellation between images. While
`SnapshotRenderer::resourcesPending` reports loading materials or models, the call re-checks every 50 ms for
up to 3 s, then renders anyway with a `RESOURCES_LOADING` warning. Without a renderer the tools fail with
`UNSUPPORTED_IN_HOST`.

**Renderer** (`ui::McpSnapshotRenderer`, TbMcpUiLib). It depends only on the `gl::GlManager` and the document,
so the headless mode (E16) can reuse it; only `userViews`/`captureUserView` need the map window.
- GL: its own `QOffscreenSurface` and `QOpenGLContext` that shares `QOpenGLContext::globalShareContext()`
  (standalone 2.1 compatibility context without one), a `QOpenGLFramebufferObject` (depth/stencil, 4× MSAA)
  read back with `toImage()`. Per render: make current (errors instead of assertions if GL is missing),
  initialize the `GlManager` if needed, check the size against the GL limits, upload the document's pending
  resources, draw, free pending VBOs and fonts, restore the previously current context.
- Independent state: every request builds its own `ObjectRenderer`s from the scene with an MCP-owned
  `mdl::EditorContext` that ignores the nodes' hidden state (`setIgnoreHiddenState`, §15) and a brush filter
  that draws all faces of the listed brushes that pass the face filter; highlighted nodes go to a tinted
  renderer set up like the editor's selection renderer. The document's `MapRenderer`, the map's
  `EditorContext`, the selection and the cameras are never used. Entity links use a link renderer restricted
  to the scene's entities; point files, axes and the 2D grid are drawn as in `MapViewBase`; markers are
  handles with a label. `toAgentCamera` converts the editor's cameras back (tested without GL); `makeGlCamera` is
  the core's (`CameraProjection.h`).
- User views: the typed `findChildren<MapViewBase*>()` of `MapWindow::mapView()`; the view id comes from the
  camera axis. Capturing calls `QOpenGLWidget::grabFramebuffer()`, which repaints the view once.
- Timing: 5,000 brushes at 1024×768 take about 180 ms for the first render (shader setup) and about 35 ms
  after that (Mesa, Intel Iris Xe).

---

### 10.13 Spatial understanding (E12)

`view_pick` and snapshot annotations are part of agent vision (§10.12).

**Empty-space analysis** (`SpaceAnalysis.h`, tested directly over `mdl::MapFixture`). `makeGrid` / `rasterize` build a
`VoxelGrid` over a region (default: the bounds of the solid brushes padded by one cell); brushes are rasterized one
by one over the cells their bounds overlap (cuboids by their bounds, other brushes with `intersectsInterior`), so
large maps stay fast. `brushRole` classifies brushes:
- *solid for spaces*: world, `func_group` and `func_detail*` brushes (not `func_detail_illusionary`) that are not
  tool-only or liquid-only; doors, `func_wall` and triggers are reported as objects;
- *sealing for leaks*: world and `func_group` brushes that are not tool-only or liquid-only; sky seals, clip, hint,
  skip, trigger and origin do not, `func_detail` does not (ericw / VHLT);
- *blocking for the player*: the solid brushes plus clip and solid brush entities (`func_wall`, `func_plat`, ...);
  doors, triggers, `func_illusionary` and water do not block.

`analyzeSpaces` returns a `SpaceMap` that holds no node pointers (safe across deferred steps). Segmentation: the
empty cells are eroded (chessboard distance) by `floor(openingSize / cellSize / 2)` cells, so an opening of n cells
across its smaller side separates spaces when `ceil(n / 2)` is at most that (`openingSize` acts rounded down to a
multiple of twice the cell size); rooms not larger than that in every direction have no core (`hasCore` false),
and when no space has one the space tools warn `OPENING_SIZE_TOO_LARGE`. The connected cores grow back 26-connected up
to the erosion distance (the inner bounds), then one step into the openings; long leftover passages become their
own spaces, leftovers connected to the outside become void, everything else grows 6-connected, and small isolated
leftovers become pockets. Boundaries between two grown regions are openings (`doorway` when they reach the floor,
`window`, `hole`; the `func_door`s in them). A core connected to the outside is a leaking room (`sealed: false`)
if most of its cells are enclosed in at least five directions, otherwise outdoor void. Space ids are an FNV hash of
the inner bounds in cells: they survive unrelated edits and change when a surrounding wall moves; they depend on
the segmentation, i.e. are only valid with the same `cellSize` and `openingSize`. The doors of an opening are the
`func_door*` brushes whose bounds touch the opening's box widened by one cell along its axis. The default cell
size is half the player width rounded to a power of two (16 in Quake).

`predictLeaks` floods the sealing grid from outside (cell size 8, doubled until the grid has at most 1M cells;
about 10 ms for a 20-room map in Debug). Only point entities with an `origin` in exported layers are checked; an
entity whose cell overlaps a brush uses the nearest free neighbour cell a straight line from the origin reaches, and
entities inside sealing brushes are not reported. The gap is where the flood path from the entity back to the
outside first reaches a cell that sees solid in fewer than four of the six axis directions; `gapBrushes` lists up to
eight sealing brushes nearest to it. Entities outside the grid or in cells that are not enclosed get no gap.
`findFreeSpots` checks every candidate against the real brushes with `intersectsInterior` and against point
entities with their model bounds; `wallDistance` applies to space-solid world and `func_group` brushes and to brushes
whose innermost group's bounds contain the candidate's center (a room built as a group), `objectDistance` to
everything else (point entities, brush entities, `func_detail`, groups that do not enclose the spot); `planWalk`
fits the player box with its lowest `stepHeight` units ignored, finds floors with five rays, and moves to the four
neighbouring columns (step 18, jump 45 for all games; 63 is a Half-Life crouch jump).

Tools (`SpaceTools.cpp`; all read-only and asynchronous with progress, cancellation between steps):
- `spaces_list {region, cellSize, openingSize (96), detail summary|full, limit (100)}` → `{cellSize, openingSize,
  count, spaces, openings, outsideOpenings, truncated}`. A space: `{id, bounds, size, floor {min, max, typical},
  ceiling, height, floorArea, volume, sealed, openings, neighbours, layers, groups, objects {pointEntities,
  brushEntities, groups, patches, classnames}, contents (full)}`; an opening: `{id (opening:n, valid in this result),
  kind, spaces [a, b | "void"], center, bounds, width, height, bottom, normal, doors}`. Warns `SPACES_NOT_SEALED`.
  About 250 ms (Debug) for 20 rooms.
- `surroundings {point, radius (512), limit (20), diagonals, maxDistance (4096), includeSpace, cellSize,
  openingSize}` → `{point, space, inside, floor, ceiling, walls [{direction, distance, face, material, object,
  entity}], objects [{id, kind, label, position, distance, direction, dz}], objectsTruncated, description}` (compass
  directions: +Y north, +X east).
- `free_spots {size, placement floor|wall|ceiling|any, space, region, includeOutside, wallDistance, objectDistance,
  step, support (1), rotate, heightAboveFloor, limit (10), sort spread|near, near}` → `{spots [{min, max, center,
  origin, size, space, floor, clearance {-x, +x, -y, +y, down, up}, wall {face, brush, normal, material,
  heightRange}, rotated}], count, candidates, step, coarsened}`. An unknown space is `OBJECT_NOT_FOUND`; no result
  warns `NO_FREE_SPOT`.
- `walkable_plan {start | from, region, cellSize, heightRange, stepHeight (18), jumpHeight (45), playerWidth,
  playerHeight, format text|image|both, maxAreas}` → `{text, legend, origin, cellSize, columns, rows, player, start,
  walkableCells, outsideCells, reachableCells, oneWayCells, reachableArea, unreachableAreas, spacesReached, image}`;
  the legend is `S . v D , - # o ' '` (start, reachable, reachable without a way back, door, walkable but
  unreachable, drop, blocked, outside, void); the PNG is drawn on the CPU. Without a start it uses the player
  start; warns `NO_START`, `START_NOT_ON_FLOOR`.

**Z-fighting** (`PlacementChecks.h`): `findZFighting(map, brushes of interest)` ports the reference rules: visible
faces of different brushes that lie in the same plane, face the same way and overlap by more than 1 unit², unless
an opposite-facing coplanar face of another brush covers the overlap. Faces with tool materials (`isToolMaterial`:
names that are not rendered, such as clip*, trigger*, caulk, nodraw, skip, hint, origin, compared on the name
after the last `/`, case-insensitive) and `trigger_*` brushes are ignored. Candidates come from the world octree;
`NodeTree::find_intersectors` returns everything in the touched cells, so results are filtered by bounds (whole map
with 2,000 brushes: 250 ms).

**`issues_list`** (`ValidationTools.cpp`, read-only, paginated): the editor validators' issues
(`registeredValidators()`, `node->issues()`) and the MCP checks (z-fighting, entities outside the hull, model
placement, UV distortion) over the whole map or `ids`. Filters `sources` (editor, mcp), `codes` (codes or editor type
names), `ids` (objects and their contents; a face issue matches its brush), `includeHidden`. Items are `{id, source,
code, type, description, objectId, face, lineNumber, hidden, fixes, details}`; the result adds `total`, `counts` per
code, `leakCheck {analyzed, skippedReason}` and `disabledValidators`. Leak prediction runs when requested by code or
when no code filter is given. Editor issue ids are `issue:<runtimeId>:<issueType>:<k>` (the k-th issue of that type
on the node), MCP issue ids `mcp:<signature>` (the per-call signature, e.g. `mcp:MODEL_FLOATING|entity:12|`; the
second field is the object the check runs on). `fixes` are the names of the fixes `issue_fix` can apply. The MCP
checks are not registered in the editor's validator list, so the human's issue browser is unchanged.

**`map_check`** (`MapCheckTools.cpp`, read-only, asynchronous, paginated): agent-oriented checks beyond the editor's
validators, each finding with a severity, a plain description and a `suggestedFix {description, tool, args}` naming the
MCP call that fixes it (tool null when no single call does). Input `checks` (placement, player_start, links, materials,
rooms; default all), `ids` (objects and their contents; player_start is map-level and runs with ids only when listed),
`cellSize`, `openingSize`. Items `{id (check:<code>:<object or signature>), check, code, severity, description,
objectId, objectIds, position, details, suggestedFix}`; the result adds `total`, `counts`, `checksRun`, `skipped
[{check, reason}]`. Each check is a deferred step (progress per check, cancellation between checks); the space analysis
runs once per call, only when a check or a suggested move needs it, and fixes use at most 25 free-spot searches.
- *placement*: models that load get the `modelPlacementIssues` codes; other standing classes (info_player_*, monster_*,
  item_*, weapon_*, ammo_*, classes with a model that are not light/env_/ambient_/path_/target_/trigger_/misc_/func_/
  info_…) are checked with their definition box (`ENTITY_IN_SOLID` below the floor or in brushes, `ENTITY_FLOATING`
  more than 16 units above the floor or without floor); other classes get `ENTITY_IN_SOLID` only when their origin
  lies at least 1 unit inside a space-solid brush; flying and swimming monsters and entities in liquids may float;
  position-independent classes (info_null, info_notnull, info_target, info_landmark, info_compile_parameters,
  info_texlights, light_environment) and logic classes that work anywhere (game_*, multisource, multi_manager,
  trigger_relay, trigger_auto, trigger_changetarget, trigger_counter, scripted_sentence, env_global, env_render,
  env_fade, env_message and the Quake 2/3 relay, delay, message and score target_* classes) are skipped. The class
  rules are `placementRule` (`EntityModelUtils`), shared with the model placement checks; the definition box is
  checked with the 1 unit tolerance.
- *player_start*: start classes from the definitions (prefixes info_player_start, info_player_deathmatch,
  info_player_coop; fallback info_player_start / info_player_deathmatch); `MISSING_PLAYER_START` suggests
  `entity_create_point` on a free floor spot of the largest sealed space; `MISSING_SINGLE_PLAYER_START` (info) when
  the game defines info_player_start and only other starts exist.
- *links*: link keys from the definitions (target/killtarget/targetname without one) and Half-Life multi_manager
  keys; `LINK_TARGET_MISSING` (fix: the existing name that differs only in case, else remove the key),
  `LINK_SOURCE_MISSING` for classes that wait for a trigger when named (func_door*, func_train, target_speaker and
  the classes below; a name counts as referenced if any property value or multi_manager key equals it),
  `NEEDS_TARGETNAME` for trigger_relay, trigger_counter, multi_manager, path_corner, info_teleport_destination and
  target_* (except target_location, target_speaker, target_cdaudio).
- *materials*: `MISSING_MATERIAL` per material name (info for tool materials) with a `material_replace` to a similar
  loaded material, else a pointer to `materials_collections_set`.
- *rooms*: `ENTITY_OUTSIDE_HULL` (leak prediction) and `ENTITY_OUTSIDE_SPACES` (the origin's cell and its neighbours
  belong to no space; fix: move into the nearest space). With a gap, the fix is a `brush_create_box` over the hole
  when the free region around the gap, in the slice across the leak path, is bounded and widens further inside
  (a local voxel grid of the sealing brushes, 16 cells around the gap): the hole's cells over the wall's thickness,
  one cell into the rim, recessed by a unit on both sides against z-fighting, with the first non-tool material of
  the gap brushes. A gap that is no such hole (a missing wall) has no suggested fix and says so in the description;
  without a gap the fix moves the entity into the nearest room.

**`issue_fix`** (`Mutation::Map`, destructive): the issues are named by `issues` (ids), `codes` and/or `ids`
(objects), `includeHidden` for hidden editor issues matched by code or object; `fix` names the fix
(case-insensitive), otherwise each issue gets its only fix and issues with several fixes are skipped. The whole call
is one undo step `AI: Fix Issues`; the change report lists every deleted and changed object.
- Editor issues are grouped by quick fix and applied like the Issues view does: the issues' objects are selected
  (`Issue::addSelectableNodes`) inside `withTargets`, then `IssueQuickFix::apply`. A fix invalidates the issues of
  the objects it changes (the `Issue` objects are freed), so each round applies the fix to at most one issue per
  object and the rest are found again by node, type, description, face index and property key; world issues are
  fixed alone (the Issues view selects nothing when a world issue is selected). Covered fixes: Delete Objects,
  Delete Property, `Replace " with '`, `Replace \ with /`, Snap Vertices, Move Brushes to World, Remove Mod, Reset
  UV Scale, Truncate Property Values.
- MCP fixes run another map tool inside the call (`runNestedTool`: schema validation and the handler, same
  context and transaction): `Apply Suggested Move` (model placement codes with `details.suggestedMove`:
  `objects_move`) and `Apply Suggested UV Fix` (`UV_ASPECT_DISTORTION`: the `details.fix` call). `Z_FIGHTING` and
  `ENTITY_OUTSIDE_HULL` have no fix; with codes or objects their checks do not run for `issue_fix`.
- After the fixes every issue is looked up again; the result is `{fixedCount, fixed, applied [{fix, code, count}],
  notFixedCount, notFixed [{id, code, objectId, reason}] (≤ 100)}`. Reasons: no fix, several fixes, the fix does not
  apply, still present after the fix, no such issue, the nested tool's error. Nothing fixed warns `NOTHING_FIXED`;
  ids that are all unknown are `OBJECT_NOT_FOUND`; no `issues`, `codes` or `ids` is `INVALID_ARGUMENT`.

**`issue_hide` / `issue_show`** (`Mutation::External`, not undoable, like the editor): `Map::setIssueHidden`, which
hides an issue type per object and is not saved in the map. Same targets as `issue_fix`; the result is
`{hidden | shown, unchanged, skipped}`; MCP issues are skipped (their checks are turned off with
`validators_set`). The issues resource is updated; the editor's Issues view shows the change at its next refresh.

**`validators_list` / `validators_set`**: the editor's validators (name = `issueCode(description)`, one code) and
the MCP checks as validators (`mcpChecks()`: `Z_FIGHTING`, `ENTITY_OUTSIDE_HULL`, `MODEL_PLACEMENT` for the four
`MODEL_*` codes, `UV_ASPECT_DISTORTION`), each `{name, source, title, codes, enabled, fixes}`. `validators_set`
(`Mutation::External`, dry run) takes `enable`, `disable` (names or titles, case-insensitive; unknown names are
`INVALID_ARGUMENT`) and `enableAll`, and returns the list with `changed`. The setting is per document
(`DocumentState::disabledValidators`) and lasts while the document is open; it does not change the editor's Issues
view. Turned-off validators are skipped by `issues_list`, `issue_fix` by code or object and the issues resource;
`ChangeCollector` drops their editor issues from `issuesIntroduced`, and `PlacementTracker` does not run turned-off
MCP checks (`PlacementTrackerOptions::disabledValidators`).

**Map manifest** (`MapManifest.h`, `ManifestTools.cpp`). `<name>.mcp.json` next to the map (`/x/foo.map` →
`/x/foo.mcp.json`): `{format: "trenchbroom-mcp-manifest", version: 1, spaces [{id, name, purpose, notes, bounds}],
keyPoints [{name, position, note}], notes [..], cameras [{name, camera}]}`; unknown top-level members are kept. It is
read lazily, written atomically (temporary file and rename), and an invalid file is an `IO_ERROR` that saves never
overwrite (`overwriteInvalid` does). The `ManifestStore` lives in `DocumentState`; the manifest of a never-saved map
stays in memory (`pending`) and is written on the first save. Saves are observed through
`MapDocument::documentWasSavedNotifier` (the editor's Save as well as `document_save` / `save_as`); save-as carries
the manifest to the new name, and the save tools warn `MANIFEST_NOT_WRITTEN` on failure.
- `map_manifest_get` (`Mutation::None`): `sections`, `restoreCameras: true | [names]` (loads saved cameras into the
  session's agent cameras, `CAMERA_LIMIT`, `UNKNOWN_CAMERA`). Returns `{path, exists, pending, spaces, keyPoints,
  notes, cameras, restoredCameras}`.
- `map_manifest_set` (`Mutation::External`, dry run): `spaces` merged by id (null removes a field), `keyPoints`
  merged by name (new ones need a position), `notes` appended unless present, `replace [sections]`, `saveCameras:
  "all" | [names]`, `remove {spaces, keyPoints, notes, cameras}`, `overwriteInvalid`. Returns `{path, written,
  pending, changed, removed, notFound, savedCameras, counts}`; warnings `MANIFEST_PENDING`,
  `MANIFEST_ENTRY_NOT_FOUND`, `MANIFEST_OVERWRITTEN`.

### 10.14 User views and camera (E14)

The camera, view option and layout tools in `ViewTools.cpp` change what the user sees; agent cameras and snapshots
(§10.12) stay separate. They reach the editor through `ViewHost` (`McpHost::viewHost()`; nullptr →
`UNSUPPORTED_IN_HOST`; a document without a window → `OPERATION_FAILED`): `views` lists the views of the document's
window (3d, xy, xz, yz, all existing in every layout; `visible` = shown in the layout) with their cameras as
`AgentCamera`s; `setCamera` sets a view's `gl::Camera` without animation; `layout` / `setMaximizedView` read and toggle
the maximized view. `ui::McpViewHost` finds the `MapViewBase` widgets of the window's `SwitchableMapViewContainer`.

- `camera_get`: the views with camera, size and visibility, the layout (`panes`, `maximizedView`, `currentView`),
  `link2dCameras`, the field of vision preference and the point file position if one is loaded.
- `camera_set`: `view` (default 3d), `position`, one of `lookAt` / `direction` / `yaw`+`pitch`, `up`, `fov`, `zoom`.
  A 2D camera keeps its position along the view axis, and with the "Link 2D cameras" preference the other 2D views
  follow (reported as `linkedViews`). The 3D field of view is the global "Field of vision" preference;
  `camera_set.fov` overrides it on the view until that preference changes or the views are recreated.
- `camera_focus`: `ids`, `box`, `point` or the selection; frames the targets with `frameBox` in the 3D view (keeping
  its direction) and centers the 2D views, keeping their zoom unless the box does not fit. It does not select
  anything and also focuses hidden or locked objects.
- `camera_step_pointfile`: next, previous, first, last or current; advances `MapDocument::pointTrace()` and places
  the cameras like `MapView3D::moveCameraToCurrentTracePoint` (point + 16 z, along the path, up +Z) without animation;
  at either end `moved: false` with an `END_OF_TRACE` warning.
- `view_options_get` / `view_options_set`: Qt-free in the core. The `Map view/*` preferences (face mode, shading,
  fog, edges, classnames, bounds, point entities and models, brushes, patches, soft bounds, entity link mode) are set
  with `setPref` exactly as `ViewEditor` does (global, all windows); tag and entity class visibility go through the
  document's `EditorContext` (`setHiddenTags`, `setEntityDefinitionHidden`; classnames, globs or class groups);
  `restoreDefaults` resets the preference options.
- `view_layout_set`: `panes` 1–4 is the global preference `Views/Map view layout`, which `MapWindow` observes and
  applies synchronously by recreating the views. Before that, `ViewHost::prepareForLayoutChange` clears the keyboard
  focus if a map view has it: destroying the focused view makes `MapWindow::focusChange` reach the destroyed view
  and crashes the editor. `maximized` maximizes a view or restores all. To maximize a view the
  host makes it current with `MapViewBase::setIsCurrent` (the activation tracker only does so on focus-in, which needs
  an active window), cycles a shared `CyclingMapView` pane to it with `MapView::cycleMapView`, and calls
  `MapWindow::toggleMaximizeCurrentView`.

All setters are `Mutation::External` and honor dry runs.

### 10.15 Preferences (E14)

`PreferenceCatalog.cpp` holds an explicit table of the editor's static preferences. Each entry references its
`Preferences::` variable and carries a category (view, renderer, colors, camera, controls, editor, browser, updater,
keyboard), a description, and allowed values, a range or a note where known. `tst_PreferenceTools` compares the table
against every preference path in `prefs/Preferences.h`, so a new upstream preference fails the test. The occluded move
trace color shares its path with the move trace color and is listed once. `gamePreferences()` adds each configured
game's path, default engine and compilation tool paths (category games), and `PreferenceHost` adds the host's own:
`ui::McpPreferenceHost` supplies the MCP preferences (category mcp) and the shortcut preferences of the action
manager's menu and map view actions and, for a document, of its tag and entity definition actions (category
keyboard; those `Action`s live in the host until the next call). The MCP server's enabled, port, bind address and
access token preferences carry a `lockedReason`: changing them stops or restarts the server, which would destroy it
inside the call. The access token is `secret`.

`preferences_get` (paginated; `paths`, `prefix`, `category`, `query`, `modifiedOnly`) returns `path, type, value,
modified, category, description` and constraints; `detail: "full"` adds `default`, `persistence`, `source`, `note` and
`lockedReason`. `preferences_set` (`values`, `reset`; `Mutation::External`) validates every change (type, allowed
values, range, read-only, locked) before applying any with `setPref`, which saves immediately; it warns about shortcut
conflicts (`SHORTCUT_CONFLICT`) and missing game paths, and notifies the game config resource when a game preference
changes. Colors are `#RRGGBB` or `#RRGGBBAA` (also accepted: `[r, g, b, a]` and TrenchBroom's `"r g b a"`); shortcuts
are arrays of portable key sequences. The editor applies shortcuts, the layout, the field of vision, fonts and render
settings immediately; the theme needs a restart (`note`).

### 10.16 User manual (E14)

`McpHost::manualPath()` returns the generated `manual/index.html`. `Manual.cpp` parses the pandoc output inside
`article#content_body` into sections (h1–h6 with ids, parents and children), each with its own text as Markdown:
lists, tables, code blocks, figures, definition lists, links and decoded entities. The manual's `print_menu_item` /
`print_action` / `print_key` scripts become placeholders, resolved on every call: menu paths come from the document
window's `ActionHost` if there is a document, else from the `shortcuts.js` next to the manual, else from the path;
shortcuts come from the action host, else the current shortcut preferences, else `shortcuts.js`. Pandoc wraps some
script arguments over lines; the parser joins them. The parse is cached by path, size and modification time; the real
manual has 182 sections and 250 references, all resolved.

`manual_search` (`query`, paginated) ranks sections by title and text hits (case-insensitive substrings; phrase and
exact-title bonuses) and returns `id, title, level, path, score, snippets, uri`. `manual_section` (`section`: id,
`#id`, exact title or resource URI; `offset`, `maxChars`, `includeSubsections`) returns the text, paged at line
breaks, with the subsections and the parent, previous and next sections. Without a manual the tools fail with
`UNSUPPORTED_IN_HOST`; an unreadable file is `IO_ERROR`.

---

## 11. Testing

### 11.1 `TbMcpLibTest` (headless, no Qt)

| Test file | Covers |
|---|---|
| `tst_Json`, `tst_JsonVm`, `tst_JsonRpc` | conversion, rounding, parse/serialize, ids, batch gating, error codes |
| `tst_McpServer` | initialize/version negotiation, capability gating, ping, `notifications/initialized` ordering, cancellation, progress |
| `tst_HttpParser`, `tst_HttpResponse`, `tst_StreamableHttp`, `tst_SseParser` | split packets, limits, status codes, sessions, SSE framing, Origin/Host checks over a fake connection |
| `tst_BridgeSession` | stdio bridge: routing (offline methods vs. editor), offline answers equal to the editor server's (initialize, lists, prompts), handshake replay, `list_changed` on connect and disconnect |
| `tst_Schema`, `tst_Args`, `tst_Errors`, `tst_Pagination` | JSON Schema output, validation errors with paths, defaults, cursors, fields |
| `tst_ToolRegistry`, `tst_ResourceRegistry`, `tst_PromptRegistry`, `tst_Resources` | listing, paging, dispatch, subscriptions, coalesced updates |
| `tst_Prompts` | the nine prompts with their arguments, missing required arguments, inserted and default values; every tool named in the prompts and the agent guide is registered |
| `tst_ToolCatalog` | every registered tool: a title, a description of at least 60 characters starting with an upper-case letter, at least one JSON object after "Example" and every such object accepted by the input schema, an object input schema with a description on every property (nested objects, array items, oneOf branches), an output schema |
| `tst_ObjectIds`, `tst_Targets` | id format/parse; delete→undo→same id; redo; linked-group aliasing; reload remap; target resolution |
| `tst_CallRunner` | one undo step `AI: …`; rollback leaves `modificationCount` and the undo stack unchanged; dry run leaves no trace and keeps the redo stack; explicit transactions and nesting; busy gate and timeout with `FakeHost` + `FakeScheduler`; image content blocks; asynchronous read-only calls (immediate while busy, concurrent, cancel, session close, document close, no undo step) |
| `tst_ChangeCollector`, `tst_CallLog` | reduction, introduced issues; ring buffer, JSONL rotation |
| `tst_<Domain>Tools` | one test case per tool file, one `SECTION` per tool: success, invalid input, dry run, explicit ids vs selection |
| `tst_CompileUtils`, `tst_CompileLog`, `tst_CompileTools` | presets for the real game configurations (only variables the game defines), task JSON round trips and errors, tool path checks; log analysis with sample VHLT, ericw, tyrutils, q3map2 and Quake 2 logs and every runner line; the compile tools over `FakeCompileHost`: success, failure, cancel, test mode, one run per document, output paths, leaks, document close, the log resource, point and portal files |
| `tst_GeometryUtils`, `tst_CsgUtils` | the pure model helpers over `mdl::MapFixture`: `intersectsInterior`, `owningBrushEntity`, `classifyBrush`, `isPointEntity`, `castRay`, `checkBox`, `geometryError`, `addBrushes`, `ScopedLockOverride`; hollowing with a thickness |
| `tst_UpstreamCommandProcessor`, `tst_UpstreamMap`, `tst_UpstreamNode`, `tst_UpstreamLoadAssimpModel`, `tst_UpstreamQuickFixes` | the changes to original TrenchBroom files (§15): redo stack kept after a rolled-back transaction, command names, transaction depth, `canRedoCommand`, `runtimeId`, assimp frames named after their animations (a studio model's sequences; the model path without animations), selecting the world selects nothing (also through a worldspawn quick fix), Move Brushes to World selects no removed entity |
| `tst_AgentCamera`, `tst_Image`, `tst_SnapshotTools` | camera math, framing, orbit, eye height over a fixture room, camera JSON; image composition and diff; the snapshot tools over `FakeSnapshotRenderer`: option handling mapped into the recorded requests (hidden tags change the scene and the image, isolate, includeHidden, highlight, face tags, 2D cameras), limits, saveTo, keepAs and compare (the undo mode leaves the history unchanged), labels, progress and cancellation, the plan image, user views, unsupported hosts, no undo steps |
| `tst_ConsoleBuffer`, `tst_ConsoleTools` | bounds, sequence numbers, clear, notifiers; `console_read` filters, cursor, pagination, `dropped`, invalid input; `console_clear` with dry run; the console resource and its coalesced notifications; the per-call `console` report (`document_open` of a map with a missing WAD) |
| `tst_MaterialKnowledge`, `tst_MaterialKnowledgeTools` | kinds from names and the real Quake config, `sampleFace` on Standard and Valve faces, histograms and statistics (merge, cap, JSON), summaries and kinds from statistics, image tile detection on synthetic images and the fixture textures, notes and corpus files (round trips, cache, invalid files), profile precedence with sources and samples, mod notes over game notes; `material_corpus_scan` (replace, merge, pattern, recursion, dry run, progress, cancel, no knowledge directory), `material_notes_get/set`, `material_usage` (the panel and the tile of the fixture corpus, defaults from the selection and the map) |
| `tst_UvCheck`, `tst_UvTools`, `tst_UvWarnings` | every finding code with negatives, source gating and skip rules, the finding JSON and fixes; `uv_check` on `uv_check.map` (a stretched tile and a fractional panel), following the suggested fixes, ids vs selection, codes, pagination; `uv_align` `keepAspect`, `round`, `typical` (notes, map, default), dry run; `material_fit_geometry` followed until the face fits; the warnings of `material_apply`, `material_replace`, `face_attributes_set` and `uv_align` and their limit |
| `tst_EntityModelUtils`, `tst_EntityModelTools` | frame property discovery (`sequence`, `frame`, fixed frames, variables that change the model), animations with names and bounds per frame, world bounds with scale, `entity_model_info`, `entity_animation_set` (names, indices, unknown animations, dry run, ids vs selection, one undo step), placement findings (a sitting model reaching below the floor, standing, floating, a chair brush, no floor), `dropToFloor` with model bounds, `objects_move` warnings, `entity_placement_check` |
| `tst_CameraProjection`, `tst_Annotations`, `tst_PickTools` | camera projection round trips (perspective, orthographic, image corners); drawing primitives and font, labels, grid, compass and player at their projected places through `view_snapshot` with the fake renderer; `view_pick` on `two_rooms.map` (brush, face, normal, pixel lists, misses, `ignore`, `maxDistance`, `kinds`, visibility of the snapshot, kept snapshots, errors) |
| `tst_SpaceAnalysis`, `tst_SpaceTools` | on `spaces.map` (two rooms, a doorway with a `func_door`, a `Chair` group, a `Lights` layer): two spaces and one doorway with size and position, floor and ceiling, stable ids across an unrelated edit and new ids after moving a wall, leak prediction (sealed, a removed wall with its gap, an entity outside without a gap, timing); `spaces_list`, `surroundings`, `free_spots` (a poster on a wall with the face id and normal), `walkable_plan` text and image, invalid input |
| `tst_PlacementChecks`, `tst_ValidationTools` | z-fighting rules (overlap, hidden by a touching face, tool materials and triggers ignored, different planes); per-call reports (`Z_FIGHTING` once with both faces, dry run, `ENTITY_OUTSIDE_HULL`, model placement, UV distortion, de-duplication with tool warnings; a `[.][benchmark]` case on 2,000 brushes); `issues_list` sources, filters, paging, hidden issues, ids and fixes of MCP issues; `issue_fix` on `issues.map`: every editor quick fix (Delete Objects, Delete Property with several issues on one object, `Replace \ with /`, Snap Vertices, Reset UV Scale on six faces of one brush, Move Brushes to World, Remove Mod, `Replace " with '`, Truncate Property Values reported as not fixed, a fix of another type), issues with several fixes, by id, by object, one undo step for several codes, dry run, checks without fixes, invalid input; `Apply Suggested Move` and `Apply Suggested UV Fix`; `issue_hide` / `issue_show` (hidden issues skipped by code, MCP issues, dry run); `validators_list` / `validators_set` (listing and issues introduced per call skip turned-off validators, dry run, `enableAll`); the issues resource (content equals `issues_list`, template listed, coalesced notifications on map changes, hiding and validator changes) |
| `tst_MapCheckTools` | `map_check` on `map_check.map` (one room, an ogre in a wall, a floating soldier, a light inside a pillar, a broken target, an untriggered door, an unnamed relay, a missing material, health outside the room, no player start): every code with positive and negative cases, following each suggested fix until the finding disappears, model placement with `models.fgd`, deathmatch-only starts, multi_manager keys, a leak with a gap, `checks` / `ids` filters, pagination, progress, cancellation, invalid input |
| `tst_EngineTools` | engine tools over `FakeEngineHost`: listing per game and the document's game, save/replace (id and parameters kept), path warnings, `FILE_EXISTS`, invalid input, dry runs; launch of the only / a named / an id-selected profile with interpolated and overridden parameters, unsaved-changes warning, never-saved map, unknown or ambiguous profile, missing engine, interpolation and start failures, host without engine support |
| `tst_ViewTools` | grid; the camera tools over `FakeViewHost` (get, set by position, look-at, direction and yaw/pitch, 2D views and linked 2D cameras, focus on ids, boxes, points and the selection without changing it, point file stepping), view options on a Quake document (preferences, tags, classnames and class groups, restore defaults), layout; invalid input, dry runs, hosts without views or windows |
| `tst_ActionTools` | `actions_list` and `action_invoke` over `FakeActionHost`: filters, pagination, labels, disabled, unknown and ambiguous actions, `DIALOG_REQUIRED` and `openDialog`, `ACTION_REFUSED`, the active tool kept, dry run, `UNSUPPORTED_IN_HOST` |
| `tst_PreferenceTools` | the catalog against every path in `prefs/Preferences.h`; `preferences_get` filters, types and value forms; `preferences_set` success, type, range and allowed-value errors, read-only and locked preferences, unknown paths with suggestions, atomicity, reset, dry run, shortcut conflicts, game and host preferences |
| `tst_KnowledgeTools` | the manual parser on a pandoc-like fixture (sections, Markdown, entities, wrapped script arguments), reference resolution with and without an action host and with `shortcuts.js`, search ranking, section paging and neighbours, the manual resources, no manual |
| `tst_MapManifest`, `tst_ManifestTools` | manifest JSON round trips and invalid files; `map_manifest_get/set` merge, replace and remove, cameras saved and restored in a new session, an unsaved map's manifest written on `document_save_as`, dry run, invalid input |
| `tst_Scenarios` | scripted scenarios: S4 (inspect `rooms.map` (Valve), import its Armory group into a Standard map next to the east wall of the selected room without overlaps, missing materials reported, imported objects selected and in the current layer), S3 (replace `wall_old*` with `wall_new*` only in the Castle layer: per-material counts, an unmatched material left alone, alignment kept, one undo step), S7 (12 columns on a circle of radius 384 facing the center, a 20-step spiral staircase, one undo step each), S1 and S6 entities; E12's acceptance scenario on `spaces.map` (pick the chair in a snapshot, both spaces with their doorway, a wall spot for a poster with the face id, z-fighting and an entity outside the hull reported by the calls that caused them); S2 on `issues.map` (every issue with type, object and explanation; the codes with one fix fixed one call each with the deleted and changed objects reported; fewer issues afterwards and the rest listed with reasons; one `AI: Fix Issues` undo step per call) |

`McpToolFixture` (`TbMcpTestUtilsLib`) runs an `McpServer` with all tools over headless documents
(`ui::MapDocumentFixture`), a `FakeHost` and a `FakeScheduler`, with one initialized session:

```cpp
ui::MapDocument& create(mdl::MapFixtureConfig = {});
ui::MapDocument& load(const std::filesystem::path&, mdl::MapFixtureConfig = {});
std::string documentId(const ui::MapDocument&) const;
std::string openSession(const std::string& clientName = ..., const std::string& protocolVersion = ...);
std::shared_ptr<CapturingRequestStream> post(const std::string& sessionId, const Json& message);
Json rpc(const std::string& method, Json params = {});
Json callRaw(std::string_view tool, Json args = {});        // full CallToolResult
Json call(std::string_view tool, Json args = {});           // CHECKs success, returns structuredContent
ToolError callExpectingError(std::string_view tool, Json args = {});
// ...As(sessionId, ...) variants for other sessions
mdl::Node* node(std::string_view id, ui::MapDocument* = nullptr);
```

`call*` run pending scheduler tasks until an asynchronous call completes, advancing the clock to delayed tasks. The fixture builds on
`lib/TbMdlLib/test-utils` (`MapFixture`, `QuakeFixtureConfig`, `TestFactory.h`, `Matchers.h`) and
`lib/TbAppLib/test-utils/MapDocumentFixture`. Fixtures live in `lib/TbMcpLib/test/fixture/`:
`mcp/maps/two_rooms.map` (two rooms, a corridor, a door, a trigger, a group and a custom layer; used by scene,
spatial, selection and resource tests, and by `SceneQuestions`, which answers E3's acceptance questions with
tool calls only), `mcp/wads/cr8_a_excerpt.wad`, `mcp/wads/materials.wad` (`wall_old_a/b/c`, `wall_new_a/b`, `floor_tile`;
material and S3 tests), `mcp/maps/rooms.map` (Valve, several groups including Armory; import and S4),
`mcp/maps/crate_quake2.map` (Quake 2 import), `mcp/maps/uv_check.map` (knowledge.wad materials with UV problems), `mcp/corpus/` (a Valve and a Standard reference map and a
broken one for `material_corpus_scan`), `mcp/wads/knowledge.wad` (`k_tile`, `k_panel`, `k_trim`, `{k_decal`),
`mcp/models.fgd` with `mdl/Game/Quake/id1/progs/person.mdl` (a Quake model with the frames `stand` and `sit`)
and `mdl/Game/Quake/id1/models/cube.mdl` (a Half-Life studio model with three sequences), `mcp/maps/spaces.map` (two rooms with a doorway and a door, a chair group, a light in a custom layer; E12), `mcp/maps/no_header.map` (format detection without a header
comment), `mcp/maps/issues.map` (Standard; editor issues of most validators, z-fighting and a floating
`monster_person`; `issue_fix` and S2), `mcp/maps/map_check.map` (Standard, `materials.wad`; `map_check`
findings), and game paths in `mdl/Game/`.

### 11.2 `TbMcpUiLibTest` (Qt, `RunAllTests.cpp` QApplication, offscreen)

- `tst_McpTcpTransport.cpp`: listen on port 0, drive with `QTcpSocket`/`QNetworkAccessManager`, wait with
  `QTest::qWaitFor`.
- `McpUiTestUtils.{h,cpp}`: showing a window under the offscreen platform fails on OpenGL, so
  `createMapWindow` registers an unshown `MapWindow` with `MapWindowManager::addMapWindow` and
  `sendShowEvent` sends a synthetic `Show` event. `withCompilationProfile`, `startCompilation` and
  `waitForCompilation` run a compilation profile with the `CmdTool` stub in a window's compilation dialog.
- `tst_QtMcpHost.cpp`: document listing, the document notifiers (open, close, focus change, created in
  place), busy detection, `prepareForAgentEdit`, `closeDocument` (with and without a window that refuses to
  close), `isCompileRunning`, `logTarget`. Creating and loading documents in new windows are not covered.
- `tst_McpUiIntegration.cpp`: status indicator and preference pane injection (this also tests
  `PreferenceDialog::addPane`).
- `tst_UpstreamHooks.cpp`: the hooks in upstream editor classes (§15): `MapWindowManager::addMapWindow`,
  `MapWindow::mapView`, `closeDiscardingChanges` and `compilationDialog`, `CompilationDialog::running`,
  `Console::messageLoggedNotifier` and `Console::clear`, `EditorContext::setIgnoreHiddenState`,
  `launchGameEngineProfile`'s process id.
- `tst_McpServerController.cpp`: preference-driven start/stop, discovery file lifecycle.
- `tst_McpViewHost.cpp`: `McpViewHost` on an unshown window: views and visibility per layout, cameras (perspective,
  fov, 2D zoom and linked 2D cameras), maximize and restore (also of a cycling pane), layout switching.
- `tst_McpActionHost.cpp`: the E14.8 coverage check (§10.9) and end-to-end actions through the tools with the real host
  (`FakeHost::actionHostOverride`): grid size, select all / deselect all, a view filter, a tag visibility action,
  `Entities/light/Create` as one undo step, a brush entity create reported as disabled, the clip tool kept active
  across calls, a deferred action run from the event loop.
- `tst_McpPreferenceHost.cpp`: the MCP and shortcut preferences (with a document's tag and entity actions), locked
  and secret preferences, setting a shortcut through `preferences_set`; the real generated manual
  (`MCP_TEST_MANUAL_PATH`, skipped if not built) parses into its sections with every reference resolved.
- `tst_McpConsoleHook.cpp`: messages of a window console reach the buffer with level and document, worker-thread
  messages are marshalled, clearing clears the views and the buffer.
- `tst_McpAnnotations.cpp`: `[gpu]` annotations drawn over real renders and `view_pick` consistency with the real
  renderer.
- `tst_McpSnapshotRenderer.cpp`: camera conversion, JPEG encoding, user views without a shown window (error
  paths); the `[gpu]` smoke test renders `two_rooms.map` with the real renderer and checks that the image is
  not mostly background and stable, that toggling the trigger changes it, that hiding the trigger in the editor
  does not, highlight, face filter, wireframe, flat, orthographic with grid, invalid sizes; `view_snapshot`
  end to end through `McpToolFixture` with the real renderer (hiding the trigger tag changes the image); a
  5,000-brush timing. The GPU tests `SKIP` when no GL context can be created; Qt's offscreen platform uses
  GLX/EGL when a display is available, so they run under `QT_QPA_PLATFORM=offscreen` on a desktop.
- `tst_McpCompileHost.cpp`: `McpCompileHost` with the `CmdTool` stub (`--printArgs`, `--exit`, `--crash`):
  success, failure, crash, cancel, test mode, export of unsaved changes, tool variables, copy tasks, reload,
  destroying a running job.
- `tst_McpEngineHost.cpp`: `McpEngineHost` with the `CmdTool` stub (`--printArgs` into a log file): interpolation of
  `${MAP_BASE_NAME}`, a malformed spec, a launch with the profile's and with overridden parameters (process id > 0,
  printed arguments), a missing engine. No real game is started.
- `tst_McpCompile.cpp`: the compile tools over `McpToolFixture` with the real `McpCompileHost`
  (`FakeHost::compileHostOverride`) and `CmdTool` as the Quake tools: a preset in test mode resolves the tool
  variables, a profile compiles unsaved changes and reports the compiled and copied files, failure, cancel,
  and a leak whose point file `pointfile_load` loads.

### 11.3 Other

- `app/TrenchBroomMcp` holds only wiring; its parsers are covered in `TbMcpLibTest`.
- Coverage: `-DTB_ENABLE_GCOV=1` per AGENTS.md; every tool file covers its error branches.
- Commands: `cmake --build <build> --target TbMcpLibTest && ctest --test-dir <build>/lib/TbMcpLib/test -j`.
- Smoke test of the real editor: `QT_QPA_PLATFORM=offscreen` and an isolated `HOME` whose `Preferences.json`
  sets `"updater/Ask for auto updates": false` (otherwise a modal update question blocks start-up before
  `--mcp-server` is processed).

---

## 12. Current limitations

- `Last-Event-ID` replay is not supported.
- Loading a document is synchronous and cannot be interrupted; only the steps around it honor cancellation.
- `selection_get` cursors are keyed to the modification count, so a selection-only change does not mark a page
  `stale`. A failed "tall" selector brush is only logged by the editor.
- `csg_subtract` does not map fragments to the brush they came from. `vertices_move` reports
  `hasRemainingVertices` only for vertex moves. The world-bounds error after `objects_duplicate` names copy ids
  that will not exist.
- Selection changes made by tools happen inside the call transaction and do not start a new repeat recording.
- `material_apply` and `material_replace` change brush faces only, not patches. Shader-only (Quake 3)
  materials cannot be previewed. `face_attributes_copy` does not redirect targets in linked groups as the
  editor's alt-click does.
- The Quake 2 game configuration's Clip tag names a flag `clip` that the game does not define, so the tag
  never matches.
- `object_get` does not report the layer color. `layer_create` takes no color.
- `visibility_set` `show` on a group does not show children that were hidden one by one. `isolate` and moving
  objects to a layer replace the human's selection, as in the editor.
- A dry run of `linked_group_extract` reports the ids from before the commit, and an extract closes an open
  group that is unrelated to the source. Linked groups nested in linked groups are not specifically tested.
- `clipboard_paste` does not convert between incompatible formats (`map_import` does); pasted face text
  applies only its last face; `clipboard_cut` cuts objects, not faces. `map_import` does not recreate the
  source layers.
- The bridge ignores portable mode when locating the discovery file. It links the whole tool code for its offline
  catalog (a large Debug binary). Its offline lists are those of its own build: connected to an editor of another
  build it sends `list_changed` for tools and resources, but prompts do not announce changes (`listChanged: false`).
- There is no tool that converts a map to another map format in place; `cleanup_map` guides the agent through
  `document_new` in the target format and `map_import`.
- Every `issue_fix` undo step is named "AI: Fix Issues". Editor quick fixes (e.g. Snap Vertices) also change hidden
  objects, while MCP fixes and the edit tools refuse them; a snap that splits a face is not warned about.
- UV neighbours must share an edge segment (overlapping coplanar faces are not neighbours) and seams are only
  checked between coplanar faces; density mismatches and seams are not reported for panels, decals and trims;
  the `material_fit_geometry` resize is exact for axis-aligned brushes only.
- Material statistics need texture sizes at scan time (or the current size) for repeats and alignment; a framed
  panel whose opposite edges have the same color looks seamless to the image analysis (notes and statistics
  take precedence); `material_corpus_scan` in `merge` mode counts a rescanned file twice; patches are not
  sampled; sloped faces use their projected extent.
- Placement checks compare axis-aligned boxes of a whole frame, not triangles, so a seated model always
  touches its chair (`MODEL_PENETRATES_BRUSHES`) and outstretched limbs enlarge the box; assimp and studio
  models use the first frame of each animation; the frame property is found only for direct mappings (n selects
  frame n); rays that start inside a brush pass through it. Loading a model the editor has not loaded yet
  happens synchronously on the editor thread.
- Spaces and leaks: gaps narrower than a cell can be missed; openings wider than `openingSize` merge two spaces
  and short narrow corridors are split between the rooms they connect; inner bounds can be one cell smaller when
  walls are off the grid. Wall spots only on axis-aligned faces where the whole back of the box lies on one face.
  Walking knows no crouching or swimming (water is ignored and its bottom is walkable) and uses column centers.
- Per-call placement reports: a z-fight uncovered by removing or moving a hiding brush is found by `issues_list`
  but not reported by the call; only UV aspect distortion is reported per call (the other UV codes come from
  `uv_check` and the material tools); a pure addition made while the leak cache is stale reports only leaks of the
  created entities; `issuesIntroduced` is capped at 100 items. `isToolMaterial` (`PlacementChecks`) and the tool
  material names of `SpaceAnalysis` are separate lists.
- `view_pick` casts against the current map, not the map as it was when the snapshot was taken; picks on
  `view_snapshot_user` images use default visibility, not the user's view filters; patch hits have no normal;
  the composite image of `view_snapshot_compare` cannot be picked. Annotation labels are placed greedily (some
  are dropped), grid visibility is sampled about every 8 pixels, and the default grid box can reach through a
  doorway.
- Snapshots do not draw entity decals, group links, or group bounds and classnames of objects inside groups.
  `view_snapshots_around` cannot save files, and `view_snapshot_compare` returns PNG only. The 2D grid is drawn
  at the world bounds, so an orthographic camera's far plane must reach them. Capturing a user view repaints
  it once; with the offscreen platform in tests only its error paths are covered.
- The console buffer compares document pointers; a freed address reused by a new document could attribute old
  messages to it. Messages logged before `McpServerController` exists (early start-up) are not recorded.
- `map_check`: definition boxes stand for entities whose model cannot be loaded; the standing, flying and
  trigger-only class lists are name-based (Quake, Half-Life, Quake 3 conventions); a name counts as referenced if any
  property value equals it, so links through keys the definitions do not type are not reported as broken targets;
  `ENTITY_OUTSIDE_SPACES` can report an entity in a closet smaller than the analysis resolution (a pocket); suggested
  moves ignore the entity's own current box when looking for free spots; patches are not checked for missing
  materials.
- Issue fixes: the editor's Truncate Property Values truncates to the maximum length, which its validator still
  reports (`>=`), so `issue_fix` lists such issues as not fixed; Delete Property removes the value instead. The
  missing-mod validator reports a mod only until worldspawn is validated again with the same mods. MCP issue ids
  of z-fighting pairs and UV findings change when the brushes change. `issue_hide` does not refresh the editor's
  Issues view immediately. Validator settings are not saved.
- Compile status is derived from the runner's log text; tool message formats not listed in §10.10 are not
  classified. The editor's compilation dialog does not know about MCP runs, so the user can start a dialog
  compile of the same document meanwhile. A cancelled tool process is killed without waiting (Qt logs
  "QProcess: Destroyed while process ... is still running").
- User views: camera changes are not undoable and not animated; a camera animation the user started (e.g. Focus) may
  overwrite them for its short duration. `camera_set.fov` is temporary (the permanent one is the preference
  `Controls/Camera/Field of vision`). The pane count and all preference view options are global (all windows);
  changing the pane count recreates the views, which resets their cameras. Maximizing a view in the cycling 2D pane
  cycles the pane like Cycle Map View (which focuses that pane's 2D cameras on the selection) and moves the keyboard
  focus to the view. The one-pane layout cannot maximize. `currentView` changes only when a view really gets the
  focus. `camera_step_pointfile` uses the editor's subdivided point trace (`count` is larger than the number of lines
  of the .pts file). Rendering and real focus changes of the user views are not covered by tests (they need a shown
  window with OpenGL).
- Actions: `action_invoke` undo steps are named "AI: Invoke Action", not after the action; its dry run does not run
  the action and reports no changes; an action opened with `openDialog` runs after the call returns and its result is
  not reported. Cut, Copy and Paste are disabled unless the map view has the keyboard focus (use the clipboard tools).
  Actions run in the current or given view even if that view is hidden in the layout. The editor's four entity link
  view filter actions store the link mode in the face render mode preference, so they are refused.
- Preferences: the MCP server's enabled, port, bind address and access token cannot be changed by agents. Shortcut
  conflicts are reported regardless of action context, and shortcut text is not validated in the core. Allowed values
  and ranges are known only for the catalog's preferences.
- Manual: the cache ignores changes to `shortcuts.js`; `manual_section` offsets and lengths are in bytes; search is
  substring-based (no stemming).

---

## 13. Implementation order

Each epic ends buildable, with tests passing and clang-format applied. Commits within an epic keep each
commit buildable and tested.

- **E1 — foundation.** CMake for `TbMcpLib`, `TbMcpLibTest`, `TbMcpTestUtilsLib`, nlohmann; `Json`,
  `JsonRpc`, `ProtocolVersion`, `McpServer`; `Errors`, `Schema`, `Args`, `ToolRegistry`, `Pagination`;
  resource and prompt registries; the TbMdlLib/TbBaseLib support (§6.6); `ObjectIds`, `ChangeCollector`,
  `CallRunner`, `Targets`, document targeting, `FakeHost`, `McpToolFixture`; `SessionTools`, `HistoryTools`,
  `CallLog`; `HttpParser`, `StreamableHttp`, `SseParser`, `McpTcpTransport`, `QtScheduler`;
  `McpServerController`, `QtMcpHost`, preferences and pane, status indicator, log file, `--mcp-server`;
  `app/TrenchBroomMcp`.
- **E2** DocumentTools and GameTools (`DocumentHost`), document info and game config resources, asynchronous
  tools with progress.
- **E3** SceneTools, SpatialTools, SelectionTools; summary and selection resources; coalesced notifications.
- **E4** GeometryTools, BrushEditTools, TransformTools, grid and locks.
- **E5** EntityClassTools, EntityCreateTools, EntityPropertyTools, entity-definitions resource.
- **E6** MaterialTools, FaceTools, TagTools, face targets in `Targets`, image content, materials resource.
- **E7** CompileTools (`CompileHost` over `CompilationRunner`; tests use the `CmdTool` stub like
  `tst_CompilationRunner.cpp`), compile presets per game family, compile log resource.
- **E8** Minimal upstream footprint: `TbMcpUiLib` for the editor glue, explicit upstream hooks (§15), tests
  of upstream fixes in `TbMcpLibTest`, `scripts/upstream-footprint.sh`, narrowest-context helpers.
- **E9** LayerTools, GroupTools and ClipboardTools (§10.11). `map_import` parses the file with an
  `mdl::MapReader` subclass that converts the format, filters the nodes and adds them through the paste path.
- **E10 — agent vision and editor console.** Read-only asynchronous calls and the per-call console report in
  `CallRunner`; `ConsoleBuffer`, `McpConsoleHook` and the console tools and resource (§9.1); agent cameras,
  `SnapshotTools` and the `map_plan_view` image form over the `SnapshotRenderer` seam, `McpSnapshotRenderer`
  (§10.12).
- **E11 — level-design knowledge.** `McpHost::knowledgeDirectory`; `AssetUtils`; `MaterialKnowledge` (profiles
  merged from knowledge notes, smart tags, a cached reference-corpus scan of `.map` files, the current map and
  image analysis with edge matching for seamless tiles) and `MaterialKnowledgeTools`; `UvCheck` and `UvTools`
  (`uv_check`, UV warnings in the material and face tools, aspect-preserving and typical fits in `uv_align`,
  `material_fit_geometry`); `EntityModelUtils` and `EntityModelTools` (animations with real bounds, the frame
  property, placement checks in `entity_create_point`, `objects_move` and `entity_placement_check`); assimp
  frames named after their animations (§15).
- **E12 — spatial understanding.** `view_pick` from a kept snapshot camera, snapshot annotations, space
  detection by flood fill of the empty volume, surroundings, free spots, walkability plan, z-fighting and leak
  prediction validators, per-map manifest file.
- **E13** ValidationTools (`issue_fix`, `issue_hide` / `issue_show`, `validators_list` / `validators_set`),
  `map_check`, EngineTools over the `EngineHost` seam (`McpEngineHost`), the issues resource.
- **E14** ViewTools (camera, view options, layout over `ViewHost`, §10.14), ActionTools and `ActionCatalog`
  (`ActionHost`, `ToolDef::keepsActiveTool`, §10.9), PreferenceTools and `PreferenceCatalog` (`PreferenceHost`,
  §10.15), KnowledgeTools and the manual parser (§10.16), the action coverage check.
- **E15 — agent experience.** Final agent guide text, description review, prompts (`Prompts.cpp`), scenario
  tests, manual section, lazy bridge start (the bridge answers `initialize` and the list methods from the
  registries and connects to the editor only when needed).
- **E16 — headless (v2, deferred).** `HeadlessHost` in TbMcpLib (no Qt; `MapDocument`s owned by the host)
  and a `--headless-mcp` stdio mode in `TrenchBroomMcp` that links the core directly. Tools need no changes.

---

## 14. Rules for implementation

**Rule 0 — minimal upstream footprint.** This project is maintained as a fork of TrenchBroom and must stay easy to sync with upstream. All MCP code lives in new files (`lib/TbMcpLib`, `Mcp*` files in other libraries, `app/TrenchBroomMcp`). Original TrenchBroom files are changed only when there is no other way (a critical bug fix, or a hook that cannot be added from outside), and each such change is as small as possible and listed with its reason in the "Upstream changes" section. A small explicit hook in an upstream file (for example a public accessor or query) is preferred over a workaround that depends on upstream internals (widget structure, object names or button texts, private members, brute force), because a hook fails loudly on upstream changes (a merge conflict or a compile error) where such a workaround fails silently. Tests are never added to existing upstream test files; they go into new test files, preferably in `TbMcpLibTest`.

1. No Qt includes in `lib/TbMcpLib`. No MCP protocol logic in `lib/TbMcpUiLib`.
2. Map changes happen only through `mdl::` free functions/commands inside `CallRunner`'s transaction. Never
   mutate nodes directly.
3. Every tool has a description with an example, declared input and output schemas, and tests for success,
   invalid input, dry run (if modifying), and explicit ids vs selection (if targeting).
4. Never hold `mdl::Node*` across calls. Store ids and resolve through `IdRegistry` every time.
5. Handlers never block, never open dialogs, and never call `QApplication::processEvents`.
6. Destructive intent is explicit (`overwrite`, `unsavedChanges`) and never prompts (PRD 7.1).
7. Shared helpers go in the `src/tools/*Utils` / `NodeJson` files; they must not grow into a second tool file.

---

## 15. Upstream changes

These are all changes to original TrenchBroom files (compared with the merge base with upstream `master`).
`scripts/upstream-footprint.sh` lists them with line counts and checks that a merge with the latest upstream
`master` has no conflicts. Everything else is in new files.

| File | Change | Why it is required |
|---|---|---|
| `lib/CMakeLists.txt` | `add_subdirectory(TbMcpLib)`, `add_subdirectory(TbMcpUiLib)` | the only place where libraries are added to the build |
| `app/CMakeLists.txt` | `add_subdirectory(TrenchBroomMcp)` | the only place where applications are added to the build |
| `app/TrenchBroom/CMakeLists.txt` | links `TbMcpUiLib`; copies `TrenchBroomMcp` next to the editor after building; passes it to `macdeployqt` (`-executable=`) in both macOS signing variants | `add_custom_command(TARGET TrenchBroom ...)` and the `macdeployqt` commands can only be changed in the directory that defines `TrenchBroom`; the bridge must ship inside the bundle and be signed with it |
| `app/TrenchBroom/src/Main.cpp` | include; `--mcp-server` option; `McpServerController` created after the `AppController` | the start-up hook: `QCommandLineParser::process` rejects unknown options, and nothing else can create the controller with the right lifetime |
| `lib/TbUiLib/include/ui/MapWindowManager.h`, `src/MapWindowManager.cpp` | `shouldCreateWindowForDocument()` and `createMapWindow()` are public; new public `addMapWindow()`, which `createMapWindow()` uses to register the window | the host creates and loads a document itself to capture its load messages before a window console takes them, and then needs a window for that document; no other public function adopts an existing document. The tests register unshown windows with `addMapWindow()` because showing a window needs OpenGL |
| `lib/TbUiLib/include/ui/MapWindow.h`, `src/MapWindow.cpp` | `closeDiscardingChanges()` (a flag that makes `confirmOrDiscardChanges()` skip the save prompt during `close()`); `mapView()`; `compilationDialog()` | `document_close` closes a window without the save prompt, and a window that refuses to close keeps its changes; the compile tools need the perspective camera, and the compile, document and session tools check whether the human's compilation dialog is compiling; the map view and the dialog are private members |
| `lib/TbUiLib/include/ui/CompilationDialog.h`, `src/CompilationDialog.cpp` | `running()` | the dialog's `CompilationRun` is private; its *Stop* button state is the only other indication of a running compilation |
| `lib/TbUiLib/include/ui/PreferenceDialog.h`, `src/PreferenceDialog.cpp` | `addPane(icon, name, pane)`: adds a pane and its tool bar button, which switches to the pane like the built-in buttons; the button is removed with the pane; the dialog grows to fit the pane | the "AI Agents" pane lives in `TbMcpUiLib`, which `TbUiLib` cannot depend on, and the dialog's tool bar, stacked widget and pane switching are private |
| `lib/TbMdlLib/include/mdl/Node.h`, `src/Node.cpp` | `Node::runtimeId()`: a process-unique ID from an atomic counter in the constructor | a registry keyed by `Node*` cannot be reliable: removed nodes live on in undo/redo commands and are freed at times the server cannot observe (redo stack cleared, rolled-back transactions, collation), so a freed address can be reused by a new node and a stale ID would resolve to an unrelated object; an ID assigned in the constructor is never reused (§5) |
| `lib/TbMdlLib/include/mdl/CommandProcessor.h`, `src/CommandProcessor.cpp` | `undoCommandNames()`, `redoCommandNames()`, `transactionDepth()` | the undo/redo stacks and the transaction stack are private; `history_get` lists them, and the busy gate and agent transactions need the depth |
| `lib/TbMdlLib/src/CommandProcessor.cpp` | the redo stack is cleared only when a command or transaction reaches the top-level undo stack | bug fix: a cancelled transaction (dry run, failed call, the human's cancelled gesture) cleared the redo history |
| `lib/TbMdlLib/src/Map.cpp` | `canRedoCommand()` checks `redoCommandName()` | bug fix: it checked the undo stack |
| `lib/TbMdlLib/src/LoadAssimpModel.cpp` | each frame is named after its assimp animation (a studio model's sequence); models without animations keep the model path as the frame name | every frame of an assimp model was named with the model path, so a Half-Life sequence such as `sitting2` could not be found by name; `entity_model_info` lists animations by name and `entity_animation_set` sets them by name. Parsing the sequence names from the model files in the MCP code would duplicate the loaders for each assimp format |
| `lib/TbMdlLib/include/mdl/EditorContext.h`, `src/EditorContext.cpp` | `ignoreHiddenState()` / `setIgnoreHiddenState(bool)`: when set, the group, entity, brush and patch `visible()` checks skip `Node::visible()` | the snapshot renderer draws the nodes the MCP core chose with its own `EditorContext`; the entity, group and patch renderers ask their `EditorContext` about the editor's hidden state and `visible()` is not virtual. `ObjectRenderer::setShowHiddenObjects` is no substitute: it replaces the brush filter and forces entity models on |
| `lib/TbMdlLib/src/SelectionCommand.cpp` | `doSelectNodes` reports only nodes that became selected | bug fix: selecting a node that cannot be selected (the world) listed it in the selection anyway, and it stayed there because deselecting skips unselected nodes. The property quick fixes select the issue's node, so fixing a worldspawn issue (e.g. `Replace \ with /`) left a stale world in the selection; deleting the selection then failed an assertion, and every later selection added the world again |
| `lib/TbMdlLib/src/PointEntityWithBrushesValidator.cpp` | the Move Brushes to World quick fix selects only the affected nodes that are still in the map | bug fix: reparenting removes the emptied point entity, and selecting it failed the selection's precondition (the editor aborted) |
| `lib/TbUiLib/include/ui/LaunchGameEngine.h`, `src/LaunchGameEngine.cpp` | optional trailing `int64_t* processId = nullptr` of `launchGameEngineProfile`, filled from `QProcess::startDetached(&pid)` | `engine_launch` reports the id of the started engine; the `QProcess` is local to the function, so the id is not available otherwise. Existing callers are unchanged |
| `lib/TbUiLib/include/ui/Console.h`, `src/Console.cpp` | static `messageLoggedNotifier(Console&, LogLevel, message)` fired in `doLog` (serialized, not re-entrant); `clear()` | `console_read` needs every console message with its level as it is logged; `doLog` is private, consoles receive messages directly from `LoggingHub` and `MapWindow::logger()`, and a window's load messages are flushed into its console in the `MapWindow` constructor, before outside code could attach to it. `console_clear` must clear the text view, which is private |

The tests of these changes are in `TbMcpLibTest` (`tst_UpstreamCommandProcessor.cpp`, `tst_UpstreamMap.cpp`,
`tst_UpstreamNode.cpp`, `tst_UpstreamLoadAssimpModel.cpp`, `tst_UpstreamQuickFixes.cpp`) and `TbMcpUiLibTest` (`tst_UpstreamHooks.cpp`, `tst_McpUiIntegration.cpp`); no
upstream test file is changed. The editor classes `AppController`, `LoggingHub`, `MapDocument`, `MapRenderer` and the
preferences are unchanged; the MCP code integrates with them from the outside (§1.3, §4.3, §6.1).
