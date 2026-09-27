# TrenchBroom MCP Server — Technical Design

Date: 2026-09-27 · Status: implemented for E1–E8; §13 lists the design of the remaining epics · Parent: [01-PRD.md](01-PRD.md) · Tools: [03-functional-spec.md](03-functional-spec.md) · Plan: [TASKS.md](TASKS.md)

This is the engineering blueprint of the MCP server. It describes the current design and
implementation. When the code and this document disagree, fix the code or update this document in the
same change.

---

## 0. Summary

| Topic | Design |
|---|---|
| Core library | Qt-free static library `lib/TbMcpLib`, namespace `tb::mcp`, headers in `include/mcp/`. It contains JSON-RPC, the MCP lifecycle, the HTTP/SSE protocol state machine, the registries, the call runner, the ID registry, and **all** tool implementations. |
| Editor glue | `lib/TbMcpUiLib` (links `TbUiLib`): `McpServerController`, `McpTcpTransport` (`QTcpServer`), `QtMcpHost` (implements the core's host interface), `McpCompileHost` (compiles with the editor's `CompilationRun`), `QtScheduler`, `McpPreferencePane`, `McpStatusIndicator`, `McpUiIntegration`. |
| stdio | Executable `app/TrenchBroomMcp`: a stdio ↔ Streamable HTTP proxy (Qt Core + Network). |
| JSON | nlohmann/json 3.12.0 via CPM (`cmake/dependencies/nlohmann_json.cmake`). |
| Protocol | MCP revision `2025-11-25`; also accepts `2025-06-18` and `2025-03-26`. Streamable HTTP on `127.0.0.1:47100` (configurable), endpoint `/mcp`. |
| Threading | Single-threaded. All networking and all tool execution run on the Qt main thread's event loop. Modifying calls wait while the human is busy. |
| Object IDs | Process-unique `Node::runtimeId()` rendered as `brush:1042`. IDs survive undo/redo; linked-group re-cloning is handled with an alias table. |
| Atomicity | Each map-modifying call runs in a `Oneshot` transaction named `AI: <Tool title>`, cancelled on failure or dry run. Explicit agent transactions are `LongRunning` and nest the per-call ones. |
| Change report | Collected from `MapDocument` notifiers during the call, reduced to net created/modified/removed sets plus the selection and the issues the call introduced. |
| Errors | Tool failures are `CallToolResult{isError:true}` with a structured `error` object (`code`, `message`, `objectIds`, `hint`). JSON-RPC errors are used only for protocol faults. |
| Schemas | A C++ builder DSL produces both the JSON Schema that `tools/list` publishes and the validator/decoder of the call. One source of truth. |
| Tests | `TbMcpLibTest` (Catch2), headless over `MapDocumentFixture` with `FakeHost`, `FakeScheduler` and an in-process client (`McpToolFixture`). Qt transport, host and editor integration tests are in `TbMcpUiLibTest`. |

---

## 1. Library and target layout

### 1.1 Dependency graph

```
app/TrenchBroom ──► TbMcpUiLib ──► TbMcpLib ──► TbAppLib ──► TbMdlLib, TbRenderLib, TbPreferencesLib, ...
       │               │  │              └──────► nlohmann_json (PUBLIC)
       │               │  └──► Qt6::Network, Qt6::Widgets
       └──► TbUiLib ◄──┘
app/TrenchBroomMcp ──► TbMcpLib (Qt-free HttpParser/SseParser/JsonRpc parts) + Qt6::Core + Qt6::Network
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
    Session.h               per-client state: id, version, capabilities, subscriptions, active document, streams
    HttpParser.h            incremental HTTP/1.1 request parser (Content-Length bodies only)
    HttpResponse.h          response and SSE frame serialization
    StreamableHttp.h        Streamable HTTP state machine over an abstract HttpConnection
    SseParser.h             incremental SSE parser (stdio bridge, tests)
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
    Resources.h             registerResources (§8)
    RegisterAll.h           registerAll(McpServer&): every register<Domain>Tools + registerResources
    tools/<Domain>Tools.h   `void register<Domain>Tools(ToolRegistry&)` plus helpers shared with resources
    tools/CompileUtils.h    compile presets per game family, tool path checks, profile JSON (§10.10)
    tools/CompileLog.h      compile log analysis: tasks, exit codes, errors, warnings, leaks (§10.10)
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
Qt6::Network TbMcpLib`. Its CMake file makes `TrenchBroom` depend on it and installs it on Windows and Linux;
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
- Forwards each message as an HTTP POST (`QNetworkAccessManager`), remembers `Mcp-Session-Id` and sends
  `MCP-Protocol-Version`. JSON responses go to stdout as one line; SSE responses are parsed with
  `mcp::SseParser` and each `data:` event becomes one line.
- After `notifications/initialized` it opens the GET stream and forwards its events, reconnecting after
  1 s, 2 s, 5 s.
- It finds the editor through the discovery file in the directory of `SystemPaths::userDataDirectory()`
  (`~/.TrenchBroom` on Linux, the application data location elsewhere; portable mode is not handled).
- **Editor not running** (no discovery file, or connection refused): it launches the sibling `TrenchBroom`
  executable with `--mcp-server` (`QProcess::startDetached`) and polls every 250 ms for up to 30 s for a new
  or changed discovery file (so a stale file is ignored). Then pending requests get `-32000 "TrenchBroom did
  not start"`.
- CLI: `TrenchBroomMcp [--port N] [--no-launch] [--editor PATH]`. Logs go to stderr only.
- The bridge does not interpret MCP beyond the session headers, so tools need no bridge changes.

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

`ToolDef::asyncHandler(fn)` with `fn(CallContext&, const Args&, ToolCompletion)` is only for
`Mutation::External` tools. They go through the queue, and the queue waits until the call completes. The
handler continues in steps scheduled with `CallContext::defer`, checks `CallContext::cancelled()` between
them, and calls the completion once. `ctx.progress(progress, total, message)` emits `notifications/progress`
when the client sent a `progressToken`.

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

class McpHost {  // ui::QtMcpHost, mcp::FakeHost (tests), later mcp::HeadlessHost (E14)
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

`QtMcpHost::compileHost()` returns its `McpCompileHost`, whose camera provider copies the perspective camera
of the document's map window (`MapWindow::mapView()`, §15) (used by export tasks that add an entity at the camera position).

`FakeHost` implements both interfaces with its own task and resource managers and a `GameManager` with the
games "Test", "Quake", "Quake 2", "Half-Life" and "Quake 3" (the real configurations from the fixture's
`games/` folder; game paths in `test/mdl/Game/` for Quake and Quake 2, empty folders in a temporary
directory for the others). The game manager writes compile and engine profiles to a temporary config
folder (`configDir()`), removed with the host. `singleWindow` simulates single-window mode,
`recentDocumentList` is the recent list, and closed documents stay alive. `compile` is a `FakeCompileHost`
whose `FakeCompileJob`s the test drives with `append` / `finish` (`onStart` can finish a job synchronously
like a test run, `startError` makes the start fail); `compileHostOverride` substitutes another compile host
(`TbMcpUiLibTest` uses the real `McpCompileHost`), and `supportsCompile = false` simulates a host without one.

Further sub-interfaces are added by the epics that need them: `ViewHost`, `ActionHost`, `PreferenceHost` (E11), the snapshot renderer (E12). A host that does not
implement a capability maps to `UNSUPPORTED_IN_HOST`.

### 4.4 Server state

`ServerState` holds everything tools may need; handlers reach it through `CallContext::server()` and tests
through `McpServer::state()`. It creates a `DocumentState` (`IdRegistry`, open `AgentTransaction`, resource
change hooks) for every open document whenever the document list changes, so subscriptions work before any
tool touched a document, and drops it on `documentWillCloseNotifier`. It owns the `CompileRuns` registry
(§10.10); `ServerState::isCompileRunning(document)` is true while an MCP run or the editor's compilation
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
   `doc:<n>` (documents), `run:<n>` (compile runs, E7), `issue:<runtimeId>:<issueType>:<k>` (issues, E10).
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
- Session DELETE, disconnect, **Stop agent**, or closing the document → rollback.
- The status bar shows "AI transaction open: <name>". Human edits made meanwhile become part of the agent
  transaction; the manual section (E13.5) documents this.

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
Non-editable targets (hidden, locked, inside a closed group) fail with `OBJECT_NOT_EDITABLE`; the hint names
the fix (`layer_set_state` unlock/show, or `group_open`). An explicit id of the wrong kind fails schema
validation (`INVALID_ARGUMENT`); a selection of the wrong kinds gives `WRONG_OBJECT_KIND`.

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
- Prompt names (E13): `blockout_level`, `populate_level`, `lighting_pass`, `texture_pass`, `fix_all_issues`,
  `compile_and_debug`, `explain_map`, `explain_entity`, `cleanup_map`.

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
  idempotent openWorld handler asyncHandler`. Annotations: `readOnlyHint` (Mutation::None),
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
`INTERNAL_ERROR`.

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
  tool's compact shape.
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
| `trenchbroom://guide` | agent guide (`AgentGuide` raw string in `Resources.cpp`) | static |

Templates are listed once per open document. `DocumentState` reports `DocumentAspect::{Info, Summary,
Selection, EntityDefinitions, Materials, Status}` changes. Info notifications and document open/close are immediate; all other updates go
through `ServerState::scheduleResourceUpdate` / `scheduleDocumentUpdate`, coalesced into one
`notifications/resources/updated` per resource and scheduler turn. Nothing is recorded while no session has
subscriptions (the hooks run on every map change, e.g. during drags).

Planned resources: `documents/{doc}/issues` (E10), `manual/{section}` (E11), `console` (E12).

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
| `SpatialTools.cpp` | `objects_at_point`, `ray_pick`, `space_check`, `map_plan_view` | E3 |
| `SelectionTools.cpp` | `selection_get/set/clear`, `select_all`, `select_invert`, `select_by`, `select_spatial`, `select_siblings`, `select_by_line`, `select_faces_of` | E3 |
| `GeometryTools.cpp` | `brush_create_box/shape/hull`, `room_create`, `opening_cut` | E4 |
| `BrushEditTools.cpp` | `brush_clip`, `face_extrude`, `face_extrude_new`, `vertices_move/remove/snap`, `vertex_add`, `csg_merge/subtract/intersect/hollow` | E4 |
| `TransformTools.cpp` | `objects_move/rotate/scale/shear/flip/duplicate/delete/array`, `command_repeat`, `command_repeat_clear` | E4 |
| `ViewTools.cpp` | `grid_get/set`; E11: `camera_*`, `view_*` | E4, E11 |
| `MaterialTools.cpp` | `materials_list`, `material_apply`, `material_set_current`, `material_replace`, `material_preview`, `locks_get/set` | E4, E6 |
| `FaceTools.cpp` | `face_attributes_get/set/copy`, `uv_align`, `uv_nudge` | E6 |
| `TagTools.cpp` | `tags_list`, `tag_apply`, `tag_remove` | E6 |
| `EntityClassTools.cpp` | `entity_classes_list`, `entity_class_describe`, `entity_model_info` | E5 |
| `EntityCreateTools.cpp` | `entity_create_point`, `entity_create_brush`, `entity_move_brushes` | E5 |
| `EntityPropertyTools.cpp` | `entity_properties_set`, `entity_property_remove/rename`, `entity_spawnflags_set`, `entity_defaults_apply`, `entity_links_get`, `entity_link`, `entity_color_set` | E5 |
| `CompileTools.cpp` | `compile_tools_get/set`, `compile_presets_list`, `compile_profiles_list`, `compile_profile_save/delete`, `compile_run`, `compile_status`, `compile_cancel`, `pointfile_load/unload`, `portalfile_load/unload`; the compile log resource | E7 |

Planned files: `OrganizationTools.cpp` (`layers_list`, `layer_*`, `objects_move_to_layer`, `group_*`,
`groups_merge`, `linked_group_*`, `visibility_set`) and `ClipboardTools.cpp` (`clipboard_*`, `map_import`)
in E9; `ValidationTools.cpp` (`issues_list`,
`issue_*`, `validators_*`, `map_check`, `engine_*`) in E10; `ActionTools.cpp` (`actions_list`,
`action_invoke`), `PreferenceTools.cpp` (`preferences_get/set`) and `KnowledgeTools.cpp` (`manual_search`,
`manual_section`) in E11; snapshot and console tools in E12; `Prompts.cpp` in E13.

`CompileTools.h` also declares `registerCompileResources`. Each domain header `include/mcp/tools/<Domain>Tools.h` declares `register<Domain>Tools` and the helpers
shared with resources: `documentInfo()` (DocumentTools.h); `gameConfigJson()`, `modsJson()`,
`entityDefinitionsJson()`, `materialsJson()`, `softBoundsJson()` (GameTools.h); `mapSummary()`
(SceneTools.h); `selectionDetails()` (SelectionTools.h); `materialsResource()` (MaterialTools.h).

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
  game's `initialMap` template for the format (or null).
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
  `enabled` (enabled collection paths). `entity_definitions_set` takes `type: "builtin" | "external"` and
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
- `map_plan_view` (text form; the image form is E12) classifies cells at the given height: `#` solid (world,
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
  selectable brush the opening overlaps.
- `brush_clip`: plane normal `cross(p1-p0, p2-p0)`; with 2 points `cross(b-a, axis)`; with `face` the face
  normal. "Front" is where the normal points.
- `face_extrude` groups faces by normal and extrudes each group. `face_extrude_new` reimplements the Extrude
  tool's split (outward, or inward for negative distances) and stamp logic with mdl calls, since those are
  file-local to `ExtrudeTool.cpp`.
- `vertices_move` targets the selected brushes, or else every editable brush that has one of the handles;
  positions match within 0.01.
- Subtracting with cutters that touch nothing and intersecting disjoint brushes follow the editor (brushes are
  removed) and warn `NOTHING_SUBTRACTED` / `EMPTY_INTERSECTION`. Other warnings: `NOTHING_CLIPPED`,
  `VERTICES_MERGED`, `SNAP_FAILED`, `NOT_HOLLOWED`.

### 10.6 Transforms, grid and locks

- Transforms use `withTargets` + `translateSelection` / `rotateSelection` / `scaleSelection` /
  `shearSelection` / `flipSelection`, so texture lock, entity angle updates and repeat semantics match the
  editor.
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
- `entity_create_point` snaps to the grid. `dropToFloor` casts five vertical rays (bounds center and inset
  corners, from the height of the bounds center) against visible solid and brush-entity brushes and patches
  (not triggers) and places the bounds on the highest hit; it warns `ENTITY_OVERLAPS_BRUSHES` if the bounds
  intersect brushes (`intersectsInterior`).
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
  as in `UvEditor`. `policy` is the editor's best/next/prev. Arguments of other operations are
  `IGNORED_ARGUMENT` warnings; fit with repeats needs a loaded material (`MATERIAL_NOT_LOADED`).
- `uv_nudge` changes offsets in each face's own texture axes (not camera-relative like `translateUv`),
  accounts for negative scales, and rotates by the grid angle by default.
- **Smart tags.** `SmartTag` does not expose its matcher, so `tags_list` classifies it from its printed form
  (classname, material, surfaceparm, content or surface flags) and tells content from surface flag matchers
  apart by testing a face; flag masks of names the game does not define are reported as `invalidflags`.
  `tag_apply` / `tag_remove` select exactly the targets (`withTargets` for object tags, where brush entity ids
  stand for their brushes; `withFaces` for face tags) and call `SmartTag::enable` / `disable` with an MCP
  `TagMatcherCallback` that picks `option` or the first choice with a `TAG_OPTION_CHOSEN` warning. Material
  tags cannot be removed (`UNSUPPORTED`, hint: `material_apply`). Results list the targets that carry the tag
  afterwards; created brush entities also appear in the change report.

### 10.9 Actions (E11)

`ActionHost` enumerates `ActionManager::visitMainMenu`, `visitMapViewActions` and `MapDocumentActionCache`
tag/entity actions. The path is the action's preference path; `enabled`/`checked` are evaluated with an
`ActionExecutionContext` for the target window. Dialog-opening actions come from a static allow-list in
`QtMcpHost`, checked by the E11.8 coverage test; each entry points to the matching semantic tool.

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
  `compile_tools_set`); an invalid game path is a `GAME_PATH_NOT_SET` warning. It returns the
  `compile_status` payload of the new run.
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
  `=== LEAK in hull 0 ===` / `Entity <class> @ (x, y, z)`, `Reached occupant ... at (x y z)`, `Leak file
  written to ...` and the `leaked` banners. The run state is `cancelled` (cancel requested, document closed,
  or `#### Terminated`), `failed` (a task failed or not all tasks completed) or `succeeded`. The compiled file
  is the first copied `.bsp` source (else the exported map's `.bsp` if it exists); `copiedTo` lists the
  copies; the point file is the reported leak file, else `<exported map>.pts` or `.lin`.
- **Point and portal files**: `pointfile_load` defaults to the latest run's leak file, then
  `compile/<base>.pts`, `<base>.pts` and the `.lin` variants; it returns the path (at most 1000 points), its
  length, the three point entities nearest to each end, and `leavesMapAt`, where the path, walked from the end
  inside the brushes' bounds, leaves them. `portalfile_load` defaults to `compile/<base>.prt`, then
  `<base>.prt`. Both use `MapDocument::loadPointFile` / `loadPortalFile`, so the editor shows them.

---

## 11. Testing

### 11.1 `TbMcpLibTest` (headless, no Qt)

| Test file | Covers |
|---|---|
| `tst_Json`, `tst_JsonVm`, `tst_JsonRpc` | conversion, rounding, parse/serialize, ids, batch gating, error codes |
| `tst_McpServer` | initialize/version negotiation, capability gating, ping, `notifications/initialized` ordering, cancellation, progress |
| `tst_HttpParser`, `tst_HttpResponse`, `tst_StreamableHttp`, `tst_SseParser` | split packets, limits, status codes, sessions, SSE framing, Origin/Host checks over a fake connection |
| `tst_Schema`, `tst_Args`, `tst_Errors`, `tst_Pagination` | JSON Schema output, validation errors with paths, defaults, cursors, fields |
| `tst_ToolRegistry`, `tst_ResourceRegistry`, `tst_PromptRegistry`, `tst_Resources` | listing, paging, dispatch, subscriptions, coalesced updates |
| `tst_ObjectIds`, `tst_Targets` | id format/parse; delete→undo→same id; redo; linked-group aliasing; reload remap; target resolution |
| `tst_CallRunner` | one undo step `AI: …`; rollback leaves `modificationCount` and the undo stack unchanged; dry run leaves no trace and keeps the redo stack; explicit transactions and nesting; busy gate and timeout with `FakeHost` + `FakeScheduler`; image content blocks |
| `tst_ChangeCollector`, `tst_CallLog` | reduction, introduced issues; ring buffer, JSONL rotation |
| `tst_<Domain>Tools` | one test case per tool file, one `SECTION` per tool: success, invalid input, dry run, explicit ids vs selection |
| `tst_CompileUtils`, `tst_CompileLog`, `tst_CompileTools` | presets for the real game configurations (only variables the game defines), task JSON round trips and errors, tool path checks; log analysis with sample VHLT, ericw, tyrutils, q3map2 and Quake 2 logs and every runner line; the compile tools over `FakeCompileHost`: success, failure, cancel, test mode, one run per document, output paths, leaks, document close, the log resource, point and portal files |
| `tst_GeometryUtils`, `tst_CsgUtils` | the pure model helpers over `mdl::MapFixture`: `intersectsInterior`, `owningBrushEntity`, `classifyBrush`, `isPointEntity`, `castRay`, `checkBox`, `geometryError`, `addBrushes`, `ScopedLockOverride`; hollowing with a thickness |
| `tst_UpstreamCommandProcessor`, `tst_UpstreamMap`, `tst_UpstreamNode` | the changes to original TrenchBroom files (§15): redo stack kept after a rolled-back transaction, command names, transaction depth, `canRedoCommand`, `runtimeId` |
| `tst_Scenarios` | scripted scenarios: S3 (replace `wall_old*` with `wall_new*` only in the Castle layer: per-material counts, an unmatched material left alone, alignment kept, one undo step), S7 (12 columns on a circle of radius 384 facing the center, a 20-step spiral staircase, one undo step each), S1 and S6 entities |

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

`call*` run pending scheduler tasks until an asynchronous call completes. The fixture builds on
`lib/TbMdlLib/test-utils` (`MapFixture`, `QuakeFixtureConfig`, `TestFactory.h`, `Matchers.h`) and
`lib/TbAppLib/test-utils/MapDocumentFixture`. Fixtures live in `lib/TbMcpLib/test/fixture/`:
`mcp/maps/two_rooms.map` (two rooms, a corridor, a door, a trigger, a group and a custom layer; used by scene,
spatial, selection and resource tests, and by `SceneQuestions`, which answers E3's acceptance questions with
tool calls only), `mcp/wads/cr8_a_excerpt.wad`, `mcp/wads/materials.wad` (`wall_old_a/b/c`, `wall_new_a/b`, `floor_tile`;
material and S3 tests), and game paths in `mdl/Game/`.

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
  `MapWindow::mapView`, `closeDiscardingChanges` and `compilationDialog`, `CompilationDialog::running`.
- `tst_McpServerController.cpp`: preference-driven start/stop, discovery file lifecycle.
- `tst_McpCompileHost.cpp`: `McpCompileHost` with the `CmdTool` stub (`--printArgs`, `--exit`, `--crash`):
  success, failure, crash, cancel, test mode, export of unsaved changes, tool variables, copy tasks, reload,
  destroying a running job.
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
- `object_get` does not report the layer color.
- The bridge ignores portable mode when locating the discovery file.
- Compile status is derived from the runner's log text; tool message formats not listed in §10.10 are not
  classified. The editor's compilation dialog does not know about MCP runs, so the user can start a dialog
  compile of the same document meanwhile. A cancelled tool process is killed without waiting (Qt logs
  "QProcess: Destroyed while process ... is still running").

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
- **E9** OrganizationTools and ClipboardTools. `map_import` parses the file with `mdl::MapReader` into nodes,
  filters them, converts the format, and adds them through the paste path.
- **E7** CompileTools (`CompileHost` over `CompilationRunner`; tests use the `CmdTool` stub like
  `tst_CompilationRunner.cpp`), compile presets per game family, compile log resource.
- **E10** ValidationTools and engine launch; issues resource.
- **E11** ViewTools (camera, view options, layout), ActionTools (`ActionHost`, §10.9), PreferenceTools
  (`PreferenceHost`), KnowledgeTools, action coverage check.
- **E12 — agent vision and editor console.** `McpSnapshotRenderer` (TbMcpUiLib) renders `MapRenderer` into a
  `QOpenGLFramebufferObject` with the shared GL context and returns PNG bytes, independent of any map window
  so E14 can reuse it; agent cameras with their own render state; snapshot tools; `map_plan_view` image form;
  `console_read`/`console_clear` and the subscribable `trenchbroom://console` resource.
- **E13 — agent experience.** Final agent guide text, description review, prompts (`Prompts.cpp`), scenario
  tests, manual section, lazy bridge start (the bridge answers `initialize` and the list methods from the
  registries and connects to the editor only when needed).
- **E14 — headless (v2, deferred).** `HeadlessHost` in TbMcpLib (no Qt; `MapDocument`s owned by the host)
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

The tests of these changes are in `TbMcpLibTest` (`tst_UpstreamCommandProcessor.cpp`, `tst_UpstreamMap.cpp`,
`tst_UpstreamNode.cpp`) and `TbMcpUiLibTest` (`tst_UpstreamHooks.cpp`, `tst_McpUiIntegration.cpp`); no
upstream test file is changed. The editor classes `AppController`, `LoggingHub`, `MapDocument` and the
preferences are unchanged; the MCP code integrates with them from the outside (§1.3, §4.3, §6.1).
