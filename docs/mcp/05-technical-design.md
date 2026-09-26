# TrenchBroom MCP Server — Technical Design

Date: 2026-09-26 · Status: Accepted (E1.1), implemented for E1–E3 (see §13) · Parent: [01-PRD.md](01-PRD.md) · Tools: [03-functional-spec.md](03-functional-spec.md) · Plan: [TASKS.md](TASKS.md)

This is the engineering blueprint for the MCP server. Every decision below is final unless a later
design note replaces it. Implementation agents follow it literally. When the code disagrees
with this document, fix the code or update this document in the same change.

---

## 0. Summary of decisions

| Topic | Decision |
|---|---|
| Core library | New Qt-free static library `lib/TbMcpLib`, namespace `tb::mcp`, headers in `include/mcp/`. It contains JSON-RPC, MCP lifecycle, the HTTP/SSE protocol state machine, the registries, the call runner, the ID registry, and **all** tool implementations. |
| Editor glue | `lib/TbUiLib` gets `Mcp*` classes: TCP transport on `QTcpServer`, `QtMcpHost` (implements the core's host interface), Qt scheduler, preferences pane, status bar indicator. |
| stdio | New executable `app/TrenchBroomMcp`: a stdio ↔ Streamable HTTP proxy (Qt Core + Network). |
| JSON | nlohmann/json 3.12.0 via CPM (`cmake/dependencies/nlohmann_json.cmake`). |
| Protocol | MCP revision `2025-11-25`. Also accepts `2025-06-18` and `2025-03-26`. Streamable HTTP on `127.0.0.1:47100` (configurable), endpoint `/mcp`. |
| Threading | Single-threaded. All networking and all tool execution run on the Qt main thread's event loop. Map-modifying calls wait while the human is busy. |
| Object IDs | Process-unique `Node::runtimeId()` (a new small TbMdlLib addition) rendered as `brush:1042`. The IDs survive undo/redo. Linked-group re-cloning is handled with an alias table. |
| Atomicity | Each modifying call runs in a `Oneshot` transaction named `AI: <Tool title>`. On failure or dry run the transaction is cancelled. Explicit agent transactions are `LongRunning` and nest the per-call ones. |
| Change report | Collected from `MapDocument` notifiers during the call, then reduced to net created/modified/removed sets plus the selection and the issues the call introduced. |
| Errors | Tool failures are `CallToolResult{isError:true}` with a structured `error` object (`code`, `message`, `objectIds`, `hint`). JSON-RPC errors are used only for protocol faults. |
| Schemas | A small C++ builder DSL produces both the JSON Schema that `tools/list` publishes and the validator/decoder the call uses. There is one source of truth. |
| Tests | `TbMcpLibTest` (Catch2), headless over `MapDocumentFixture` with a `FakeHost` and an in-process client. Transport tests go in `TbUiLibTest`. |

---

## 1. Library and target layout

### 1.1 Dependency graph

```
app/TrenchBroom ──► TbUiLib ──► TbMcpLib ──► TbAppLib ──► TbMdlLib, TbRenderLib, TbPreferencesLib, ...
                        │            └──────► nlohmann_json (PUBLIC)
                        └──► Qt6::Network (already PUBLIC in TbUiLib)
app/TrenchBroomMcp ──► TbMcpLib (only the Qt-free HttpParser/SseParser/JsonRpc parts are used) + Qt6::Core + Qt6::Network
```

`TbMcpLib` must link no `Qt6::` target. This is enforced the same way `TbAppLib` enforces it,
by not linking Qt. It depends on `TbAppLib` because tools operate on `ui::MapDocument`. That
class owns the `mdl::Map`, re-emits every `Map` notifier, and survives `reload()` (which
replaces the `Map`). Tools reach the map through `document.map()`, and mutations go through
the `mdl::` free functions in `Map_*.h`.

### 1.2 `lib/TbMcpLib`

This mirrors `lib/TbAppLib`: a `STATIC` library, `FILE_SET headers` with
`BASE_DIRS include`, `PRIVATE CompilerConfig PrecompileStdHeaders`, and `add_subdirectory(test)`
and `add_subdirectory(test-utils)`. The directory layout is flat, like `mdl/` and `ui/`:

```
lib/TbMcpLib/
  CMakeLists.txt            add_library(TbMcpLib STATIC); PUBLIC: nlohmann_json::nlohmann_json, TbAppLib, TbBaseLib, TbMdlLib, KdLib, VmLib
  README.md
  resources/agent-guide.md  embedded at build time (configure_file -> generated AgentGuide.h raw string)
  include/mcp/
    Json.h                  `using Json = nlohmann::ordered_json;` helpers, number rounding
    JsonVm.h                to/from JSON for vm::vec3d, vm::bbox3d, vm::plane3d, Color
    JsonRpc.h               Message variant (Request/Notification/Response/Error), parse/serialize, error codes
    ProtocolVersion.h       supported revisions, negotiation
    Endpoint.h              transport-facing interface of McpServer (post, notification stream, delete session)
    McpServer.h             protocol engine: sessions, lifecycle, method dispatch (transport-neutral)
    ServerState.h           shared state (host, registries, sessions, per-document state, call runner) seen by tools
    Session.h               per-client state: id, version, capabilities, subscriptions, agent transaction, outbound queue
    HttpParser.h            incremental HTTP/1.1 request parser (Content-Length bodies only)
    HttpResponse.h          response/SSE frame serialization
    StreamableHttp.h        Streamable HTTP state machine over an abstract HttpConnection
    SseParser.h             incremental SSE parser (used by the stdio bridge and tests)
    Host.h                  abstract McpHost + sub-interfaces (documents, busy state, actions, views, compile, prefs)
    Scheduler.h             abstract Scheduler (post, postDelayed, now)
    Schema.h                schema builder DSL + validator/decoder
    Args.h                  typed access to validated arguments
    ToolRegistry.h          ToolDef, registry, tools/list paging
    ResourceRegistry.h      static resources, templates, subscriptions
    PromptRegistry.h        prompts/list, prompts/get
    CallContext.h           everything a handler sees (document, ids, args, report, progress, warnings)
    CallRunner.h            busy wait, transaction wrapping, dry run, error mapping, logging
    ChangeCollector.h       notifier-based change report
    ObjectIds.h             IdRegistry, ObjectRef parsing/formatting, face refs
    Targets.h               resolve "ids or current selection", select-then-act helper
    Errors.h                ToolError, ErrorCode, warnings
    Pagination.h            cursor/limit/fields/detail helpers
    CallLog.h               in-memory ring buffer + JSONL sink interface
    tools/                  (headers expose only `void register<Domain>Tools(ToolRegistry&)`)
      SessionTools.h  HistoryTools.h  DocumentTools.h  GameTools.h  SceneTools.h
      SelectionTools.h  GeometryTools.h  TransformTools.h  EntityTools.h  MaterialTools.h
      OrganizationTools.h  ClipboardTools.h  ValidationTools.h  CompileTools.h
      ViewTools.h  ActionTools.h  PreferenceTools.h  KnowledgeTools.h
    Resources.h  Prompts.h  RegisterAll.h
  src/                      same names, .cpp (tools in src/tools/)
  test/                     TbMcpLibTest (tst_<Unit>.cpp, fixture/ copied like TbAppLibTest incl. games/)
  test-utils/               TbMcpTestUtilsLib: FakeHost, FakeScheduler, McpToolFixture, TestClient, JSON matchers
```

Add `add_subdirectory(TbMcpLib)` to `lib/CMakeLists.txt` (alphabetically, i.e. before `TbMdlLib`).
The test target copies `fixture/`, `games`, and `games-testing` exactly like
`lib/TbAppLib/test/CMakeLists.txt`. It links `Catch2::Catch2WithMain TbMcpLib TbMcpTestUtilsLib
TbAppTestUtilsLib TbMdlTestUtilsLib TbBaseTestUtilsLib` and calls `catch_discover_tests(TbMcpLibTest)`.

### 1.3 Additions to `lib/TbUiLib` (namespace `tb::ui`)

| File | Responsibility |
|---|---|
| `McpServerController.{h,cpp}` | Owned by `AppController` (`std::unique_ptr`). Creates `mcp::McpServer` with all registries, the `QtMcpHost`, and `McpTcpTransport`. Starts and stops on preference changes and at app start/quit. Writes and removes the discovery file. Exposes Qt signals for the UI: `clientsChanged`, `activityChanged(QString)`. |
| `McpTcpTransport.{h,cpp}` | `QTcpServer` + one `QTcpSocket` per connection, adapted to `mcp::HttpConnection`. Byte I/O only; all HTTP/MCP logic stays in the core. |
| `QtMcpHost.{h,cpp}` | Implements `mcp::McpHost` with `AppController`, `MapWindowManager`, `MapWindow::toolBox()`, `ActionManager`, `CompilationRunner`, `GameEngineProfileManager`, `PreferenceManager`, and the offscreen snapshot renderer. |
| `QtScheduler.{h,cpp}` | `mcp::Scheduler` via `QTimer::singleShot` on the main thread. |
| `McpSnapshotRenderer.{h,cpp}` | (E9) Renders `MapRenderer` into a `QOpenGLFramebufferObject` using the shared GL context and returns PNG bytes. |
| `McpStatusIndicator.{h,cpp}` | Status bar widget, added in `MapWindow::createStatusBar()` next to the update indicator. Shows "AI: n clients · <current tool> / waiting for you / idle" and a **Stop agent** button. |
| `McpPreferencePane.{h,cpp}` | New "AI Agents (MCP)" pane in `PreferenceDialog`: enable, port, bind address, access token (required only for non-loopback binding), log-to-file, busy-wait timeout. |

Preferences go in `lib/TbPreferencesLib/include/prefs/Preferences.h`, following the
existing `inline auto X = Preference<T>{...}` style:

```cpp
inline auto McpServerEnabled     = Preference<bool>{"MCP/Enabled", false};
inline auto McpServerPort        = Preference<int>{"MCP/Port", 47100};
inline auto McpServerBindAddress = Preference<std::string>{"MCP/Bind address", "127.0.0.1"};
inline auto McpServerAccessToken = Preference<std::string>{"MCP/Access token", ""};
inline auto McpLogToFile         = Preference<bool>{"MCP/Log to file", true};
inline auto McpBusyWaitTimeoutMs = Preference<int>{"MCP/Busy wait timeout", 30000};
```

`app/TrenchBroom/src/Main.cpp` gets a `--mcp-server` option (`QCommandLineOption`). It enables
the server for this process only, whatever the preference says. The stdio bridge uses it when
it launches the editor.

### 1.4 `app/TrenchBroomMcp`

This follows `app/CmdTool` / `app/DumpShortcuts`: `add_executable(TrenchBroomMcp)`,
`EMBED_UTF8_MANIFEST`, links `CompilerConfig Qt6::Core Qt6::Network TbMcpLib`, and is added to
`app/CMakeLists.txt`. Packaging installs it next to the `TrenchBroom` executable. On macOS it goes
into `TrenchBroom.app/Contents/MacOS/`. `add_dependencies(TrenchBroom TrenchBroomMcp)` keeps
them in sync. Behavior is described in §3.5.

---

## 2. JSON library

**Decision: nlohmann/json v3.12.0, fetched with CPM.** Add
`cmake/dependencies/nlohmann_json.cmake`:

```cmake
CPMAddPackage(
  URI "gh:nlohmann/json#v3.12.0"
  OPTIONS
    "JSON_BuildTests OFF"
    "JSON_Install OFF"
    "JSON_ImplicitConversions OFF"
)
```

Include it in `cmake/Dependencies.cmake`, alphabetically after `miniz.cmake`. It is header-only,
so it needs no `suppress_dependency_warnings` or `apply_sanitizer_options`, like `ctre.cmake`.

Why:

- The core must be Qt-free (the `TbAppLib` rule, and the v2 headless mode), so `QJsonDocument` is out. The repository has no JSON library today. `UpdateLib` uses Qt JSON, and that is fine for UI code.
- nlohmann is header-only, MIT, CMake-native, supports C++20, and has an `ordered_json`. Ordered keys make responses deterministic and readable for models, and make golden tests stable.
- Performance is enough. `map_tree` for 5,000 brushes serializes a few MB well under the 200 ms budget. The cost is dominated by traversal, not by JSON.
- `JSON_ImplicitConversions OFF` forces explicit `get<T>()`, which avoids silent type coercion bugs in argument handling.

Conventions: `using Json = nlohmann::ordered_json` everywhere. Vectors are `[x, y, z]`. Boxes
are `{"min":[..],"max":[..]}`. Output doubles are rounded to 6 decimals (`mcp::roundForOutput`), so
agents see `64` rather than `63.99999999997`.

---

## 3. Transport

### 3.1 Protocol revisions

`ProtocolVersion.h` lists `2025-11-25` (preferred), `2025-06-18`, and `2025-03-26`. On `initialize`,
if the client's `protocolVersion` is supported it is echoed back; otherwise the server answers
`2025-11-25`. JSON-RPC batch arrays are accepted only on sessions that negotiated `2025-03-26`;
otherwise the server returns `-32600`. Features that depend on the version, such as
`structuredContent`/`outputSchema` (2025-06-18+), are gated in `Session`.

Server capabilities: `tools{listChanged:true}`, `resources{subscribe:true, listChanged:true}`,
`prompts{listChanged:false}`, `logging{}`. `serverInfo = {name:"trenchbroom", title:"TrenchBroom",
version:<app version>}`. `instructions` is a short string that points to the agent guide resource.

### 3.2 Streamable HTTP (`StreamableHttp.h`, core)

The server has one endpoint, `http://<bind>:<port>/mcp`. The state machine consumes parsed
`HttpRequest`s from an `HttpConnection` interface (`write(bytes)`, `close()`, `id()`), so
it is fully unit-testable without sockets.

- **POST** with a JSON-RPC body. Requests must send `Accept: application/json, text/event-stream`.
  - Only notifications or responses: `202 Accepted`, no body.
  - Requests: the server replies `Content-Type: application/json` with the single response once it is ready.
    It switches to `text/event-stream` only when the call emits progress notifications (it has a
    `progressToken`) or is asynchronous (compile, open). Then it streams the notifications and the final response, and closes the stream.
    The response is deferred: the connection stays open while the call waits in the busy queue.
- **GET** with `Accept: text/event-stream`: opens the session's standalone SSE stream for server
  notifications (`resources/updated`, `tools/list_changed`, `notifications/message`). There is one per
  session; a second GET replaces the first. A `: keepalive` comment is sent every 15 s.
- **DELETE**: terminates the session (rolls back its open agent transaction, drops subscriptions).
- **Sessions**: `initialize` creates a session and returns `Mcp-Session-Id` (128-bit random hex, from
  `base/Uuid.h`). Every later request must carry it; an unknown id gets `404`, a missing one `400`.
  After initialization the `MCP-Protocol-Version` header must match the negotiated version; if it is
  absent the server assumes the negotiated one (as the spec allows).
- **Resumability**: SSE events carry `id: <session-seq>`. `Last-Event-ID` replay is **not**
  supported (a reconnecting client simply re-reads resources); this is stated in the guide.
- **Security**: reject any request whose `Origin` header is present and is not `http://localhost*` /
  `http://127.0.0.1*` (`403`). Reject a `Host` header that does not match the bind address or `localhost` (DNS
  rebinding). When bound to a non-loopback address, require `Authorization: Bearer <McpServerAccessToken>`,
  and refuse to start when the token is empty. On loopback there is no auth. This matches PRD §7.1:
  full access, and the transport default is the only limit.
- **HTTP subset** (`HttpParser.h`): HTTP/1.1, `Content-Length` request bodies (reject chunked
  request bodies with `411`), a 16 MiB body limit, keep-alive. SSE responses use
  `Transfer-Encoding: chunked`. The parser is incremental, so a request split across TCP reads works.

**Why not QHttpServer:** it is a separate Qt module that is not in our `find_package(Qt6 ...)` list
and is missing from some distro Qt builds. We need a small subset of HTTP, and keeping it in the
Qt-free core makes it testable and reusable in headless mode.

### 3.3 Qt adapter (`McpTcpTransport`, TbUiLib)

`QTcpServer::listen(QHostAddress(bindAddress), port)` runs on the main thread. On each `readyRead`
it calls `conn.feed(socket->readAll())` and the core parser handles the rest. There are no extra
threads. If the port is taken, the server logs an error to the console, the status indicator shows
"MCP: port 47100 in use", and nothing else happens (the editor keeps working).

### 3.4 Discovery file

While the server listens, `McpServerController` writes `<SystemPaths::userDataDirectory()>/mcp-server.json`:
`{"port":47100,"bind":"127.0.0.1","pid":1234,"version":"2026.1"}`. It deletes the file on stop and quit.
The bridge uses this file to find the editor.

### 3.5 stdio bridge (`TrenchBroomMcp`)

- Reads newline-delimited JSON-RPC from stdin on a `std::thread` (portable, because `QSocketNotifier`
  cannot read stdin on Windows). Each line is handed to the main thread with `QMetaObject::invokeMethod`.
- Forwards each message as an HTTP POST with `QNetworkAccessManager`. It remembers `Mcp-Session-Id` from
  the `initialize` response and adds `MCP-Protocol-Version` afterwards. JSON responses go to stdout
  as one line. SSE responses are parsed with `mcp::SseParser`, and each `data:` event becomes one line
  on stdout.
- After `notifications/initialized` it opens the GET stream and forwards its events, reconnecting with
  backoff (1 s, 2 s, 5 s).
- **Editor not running** (no discovery file, or the connection is refused): it launches the sibling
  `TrenchBroom` executable with `--mcp-server` (detached, via `QProcess::startDetached`) and polls the
  discovery file every 250 ms for up to 30 s. After that it answers pending requests with JSON-RPC error
  `-32000 "TrenchBroom did not start"`.
- CLI: `TrenchBroomMcp [--port N] [--no-launch] [--editor PATH]`. It logs to stderr only, because
  stdout is protocol.
- The bridge does not interpret MCP beyond the session headers, so new tools need no bridge changes.

### 3.6 Multiple clients

Each `initialize` creates an independent `Session`. Notifications fan out to every session
subscribed to the resource (`resources/updated`) or to all sessions (`list_changed`). Tool calls from all
sessions go through one FIFO call queue (see §4), so edits never interleave. Each session has
its own active document (default: the focused window) and its own agent transaction.

---

## 4. Threading and dispatch

**Everything runs on the Qt main thread.** `QTcpServer` callbacks, parsing, dispatch, tool
handlers, and notifier callbacks all run in the GUI event loop. Therefore:

- No locks are needed around `Map`/`MapDocument` (which are not thread-safe).
- A handler must not block. The budget is < 100 ms for simple edits; long work is asynchronous (below).

### 4.1 Call queue and the "human busy" gate (spec X13)

`CallRunner` keeps one FIFO of pending `tools/call`s. Read-only tools (`readOnlyHint`)
bypass the queue and run immediately; reading an intermediate drag state is harmless. For a
modifying call:

1. `host.busyState(document)` is checked. It returns `Busy` when any of these is true:
   - `MapWindow::toolBox().dragging()` (mouse drag or gesture tracker active; `ToolBox::dragging()`
     returns `m_gestureTracker != nullptr`);
   - `QApplication::activeModalWidget() != nullptr` (a modal dialog is open, e.g. Compile, Preferences);
   - `map.commandProcessor().transactionDepth() > agentDepth(document)`. The human has a transaction
     open: drag tools such as move/rotate/vertex open `LongRunning` transactions for the gesture.
     `agentDepth` is the depth the MCP server itself opened.
2. While the state is `Busy`, the call stays queued, and the scheduler re-checks every 50 ms. The status
   bar shows "AI waiting for you…". After `McpBusyWaitTimeoutMs` the call fails with `BUSY_TIMEOUT`.
3. When the human is not busy but a **modal tool owns the selection** (`ToolBox::selectionOwnedByTool()`,
   e.g. vertex or clip tool active), `host.prepareForAgentEdit()` deactivates the current tool (as
   Escape would) and adds the note `"deactivated tool: Vertex Tool"` to the result warnings. Waiting
   for an idle modal tool could stall forever, and its handles would become stale after our edit anyway.

`notifications/cancelled` removes a still-queued call; an executing synchronous call cannot be
interrupted, and its result is dropped. **Stop agent** (status bar): clear the queue, cancel asynchronous
operations, cancel every open agent transaction, and close all sessions (sockets closed; clients
must re-initialize).

### 4.2 Asynchronous tools

Long operations (`document_open` of big maps is still synchronous for MVP; `compile_run`,
`entity_definitions_reload`, `materials_reload`, later `map_import` of large files) use
`ToolDef::asyncHandler(ctx, args, Completion)`. The handler starts the work (e.g. `CompilationRunner`
on its existing `QProcess` signals via the host), returns immediately, and calls `completion(result)`
later on the main thread. `ctx.progress(fraction, message)` emits `notifications/progress` when the client
sent a `progressToken`. Compile runs return a `run:<n>` handle immediately (`compile_run` is
fire-and-poll with `compile_status`), so no MCP request stays open for minutes.

### 4.3 Abstract seams (for tests and headless mode)

```cpp
namespace tb::mcp {
class Scheduler { public: virtual void post(std::function<void()>) = 0;
  virtual void postDelayed(std::chrono::milliseconds, std::function<void()>) = 0;
  virtual std::chrono::steady_clock::time_point now() const = 0; virtual ~Scheduler(); };

enum class BusyState { Idle, Busy };
struct DocumentInfo { std::string id; ui::MapDocument* document; std::string windowTitle; bool focused; };

class McpHost {  // implemented by ui::QtMcpHost, mcp::FakeHost (tests), later mcp::HeadlessHost (E11)
public:
  virtual std::vector<DocumentInfo> documents() = 0;
  virtual BusyState busyState(ui::MapDocument&) = 0;
  virtual std::vector<std::string> prepareForAgentEdit(ui::MapDocument&) = 0;   // returns notes
  virtual DocumentHost& documentHost() = 0;   // new/open/save-as dialogs-free, close, revert, recent
  virtual ActionHost& actionHost() = 0;       // list/invoke ActionManager actions (E9)
  virtual ViewHost& viewHost() = 0;           // camera, view options, layout, snapshots (E9)
  virtual CompileHost& compileHost() = 0;     // profiles, runs, engines (E8)
  virtual PreferenceHost& preferenceHost() = 0; // all preferences incl. game paths (E2/E9)
  virtual const mdl::GameManager& gameManager() = 0;
  virtual std::filesystem::path manualDirectory() = 0;
  virtual ~McpHost();
};
}
```

Sub-interfaces are added in the epic that needs them. `FakeHost` returns `Unsupported` for any it
does not implement, which maps to error `UNSUPPORTED_IN_HOST`.

---

## 5. Stable object IDs

### 5.1 Findings in the code

- **Node pointers survive undo/redo.** `AddRemoveNodesCommand` keeps the removed `Node*`s (owned by
  the command) and re-adds the *same pointers* on undo; it swaps `m_nodesToAdd/m_nodesToRemove`.
  `ReparentNodesCommand` moves the same pointers. `SwapNodeContentsCommand` swaps `NodeContents`
  (entity/brush/group/patch values) *inside* the same `Node*`. Selection, visibility, and lock commands do not
  touch identity.
- **Removed nodes are freed later.** When the redo stack is cleared or a command is dropped, its
  `AddRemoveNodesCommand` destructor deletes the nodes it holds. There is no notification for this, so a new node can get
  a recycled address. **Raw pointers are therefore not safe as IDs.**
- **Linked groups re-clone children.** `UpdateLinkedGroupsHelper::doReplaceChildren` replaces all
  children of each *target* linked group with fresh clones (`Node::replaceChildren`). It fires
  `nodesWereRemoved(old)` followed by `nodesWereAdded(new)`; undo swaps the old pointers back. The clones keep
  the source's `Object::linkId()`.
- **Existing persistent ids are not general.** `LayerNode`/`GroupNode::persistentId()` (assigned by
  `WorldNode`, saved as `_tb_id`) exist only for layers and groups. `Object::linkId()` is a UUID for
  brushes/entities/groups/patches, but it is **shared by corresponding objects in linked groups** and
  copied by `clone()` (`cloneLinkId`), so it is not unique.
- `Map::reload()` builds a completely new `Map`, and `MapDocument::setMap` replaces it (followed by
  `documentWasLoadedNotifier`).

### 5.2 Decision

1. **TbMdlLib change (E1.15):** add `IdType Node::runtimeId() const`. It is assigned in `Node::Node()`
   from a `static std::atomic<IdType>` counter starting at 1. It is never copied (clones are new nodes,
   and the private copy constructor also draws a fresh id), never persisted, and never reused within the
   process. It is atomic because map parsing may construct nodes on `task_manager` worker threads. Add a test in `tst_Node.cpp`.
2. **External ID format:** `<kind>:<runtimeId>`, where kind ∈ `world | layer | group | entity | brush | patch`,
   e.g. `brush:1042`. The world is also addressable as `world`, and the default layer as `layer:default`.
   Faces are `brush:1042/face:3`: the index into `BrushNode::brush().faces()`. Other handles are `doc:<n>`
   (documents, assigned by the host per `MapDocument*`), `run:<n>` (compile runs), and
   `issue:<runtimeId>:<issueType>:<k>` (issues).
3. **`IdRegistry` (one per `MapDocument`)** maps `runtimeId → Node*` for nodes currently in the tree.
   - Build: a full tree walk on attach and on `documentWasLoadedNotifier`.
   - `nodesWereAdded`: register the nodes and all descendants. `nodesWereRemoved`: unregister the nodes
     and descendants (so a pointer is never dereferenced after it could have been freed).
   - Resolving an id that is not registered gives `OBJECT_NOT_FOUND`. The message says that the object
     may have been deleted and that `undo` may restore it. Because undo re-adds the same `Node*` with the same
     `runtimeId`, the old id becomes valid again automatically.
   - **Linked-group aliasing:** within one notifier burst (removed followed by added under the same
     parent), for every added node whose `(parent GroupNode*, path-of-linkIds)` matches a node just
     removed, record `alias[newRuntimeId] = canonicalId(oldNode)`. `formatId(node)` emits the canonical
     (first-seen) id, and `resolve()` follows aliases. From the agent's point of view, a brush inside a
     linked copy keeps its id when another copy is edited. The change collector reports such a node as
     **modified**, not as removed+added.
   - **Reload/revert:** node IDs do not survive. Layers and groups are remapped by `persistentId()`
     (exact), and all other old ids resolve to `OBJECT_NOT_FOUND` with hint "document was reloaded".
     `document_revert` returns `"idsInvalidated": true`.
4. **Faces:** a face index is valid until the brush's geometry changes. Every brush payload returns
   faces with `index`, `normal`, `center`, and `material`, so agents can re-resolve. Tools that take faces also
   accept `{"brush":"brush:1042","normal":[0,0,1]}` (the face whose normal is within 0.001 of the given one).
   The change report lists geometry-changed brushes under `modified`; the guide says to re-read faces then.
5. Every object payload also exposes `persistentId` (layers/groups) and `linkId` (for linked-group reasoning).

---

## 6. Transactions, atomicity, dry run, change report

### 6.1 Per-call transaction (`CallRunner`)

For a tool with `mutation == Mutation::Map`:

```
collector.attach(document)                          // §6.3
map.startTransaction("AI: " + def.title, TransactionScope::Oneshot)
status = handler(ctx, args)                         // uses Map_* free functions
if (!status || ctx.dryRun()) map.cancelTransaction();       // rollback + pop, no undo entry
else if (!map.commitTransaction()) status = error(OPERATION_FAILED, "linked group update failed")
report = collector.finish()
```

- `Map_*` functions open their own `Transaction`s (e.g. `"Create Brush"`). They nest inside ours, and
  committing a nested transaction folds it into the parent, so the undo menu shows exactly one
  `AI: Create box brush` entry (X2). If a call changes nothing, the empty transaction stores nothing, and the
  result says `"undoStep": null`.
- Handlers signal failure by returning `ToolError`. A `false` returned by any `Map_*` call is turned into
  `OPERATION_FAILED` with the messages captured from the document logger during the call.
  `ScopedLogCapture` re-targets the document's `LoggingHub` to a capturing logger that forwards to the
  original target. This needs a new `LoggingHub::targetLogger()` getter in TbBaseLib, added in E1.16.
- Exceptions are caught in `CallRunner`, which cancels the transaction and returns `INTERNAL_ERROR`. A server
  failure never crashes the editor (PRD 7.4).
- `Oneshot` makes the intermediate state unobservable. Since we are synchronous, no repaint happens
  between do and rollback, so a dry run is invisible on screen.

### 6.2 Explicit agent transactions (X3)

`transaction_begin{name}` calls `map.startTransaction("AI: " + name, LongRunning)` and stores
`{document, depthAtBegin}` in the `Session`. Later calls nest their `Oneshot` transactions inside it.
A failed call rolls back only itself (nested `cancelTransaction`). `transaction_commit` → `commitTransaction()`.
`transaction_rollback` → `cancelTransaction()`. Rules:

- At most one agent transaction per document. Another session's modifying call on that document fails
  with `TRANSACTION_ACTIVE`, and the error names the owning client.
- `undo`/`redo` while one is open → `TRANSACTION_ACTIVE`. (`CommandProcessor::undo()` has a
  `contract_pre(m_transactionStack.empty())`; the UI already disables undo then.)
- A session DELETE, a disconnect, **Stop agent**, or closing the document → rollback.
- The status bar shows "AI transaction open: <name>". Human edits made meanwhile become part of the agent
  transaction. This is documented in the manual section (E10.5) and is accepted behavior.
- **TbMdlLib change (E1.16):** add `size_t CommandProcessor::transactionDepth() const` (plus a
  `Map` passthrough), used by the busy gate in §4.1.

### 6.3 Change report (`ChangeCollector`, X5)

The collector subscribes to the `MapDocument` notifiers for the duration of one call:

| Notifier | Action |
|---|---|
| `nodesWereAddedNotifier` | add the nodes and descendants to `added` |
| `nodesWillBeRemovedNotifier` | format their ids *before* removal (canonical ids) |
| `nodesWereRemovedNotifier` | add them to `removed` |
| `nodesWillChangeNotifier` | snapshot the issue signatures of those nodes (for "introduced issues") |
| `nodesDidChangeNotifier` | add them to `changed` |
| `nodeVisibilityDidChangeNotifier`, `nodeLockingDidChangeNotifier` | add them to `changed` (field `stateOnly`) |
| `selectionDidChangeNotifier` | mark the selection as dirty |
| `currentLayerDidChangeNotifier`, `groupWasOpened/ClosedNotifier` | record `context` changes |

Reduction: `created = added − removed`, `removed = removed − added` (minus linked aliases), and
`modified = (changed ∪ aliased) − created − removed`. Parents whose child sets changed are included in `modified`.
Issues introduced: for `created ∪ modified` nodes, compute `node->issues(world.validators)` after the call,
minus the signatures `(type, description)` captured before the change. A dry run produces the same report
from the rolled-back execution. Its ids for created objects are marked `"ephemeral": true`, because they will
not exist.

Result envelope for every modifying tool (tool-specific data goes under `result`):

```json
{
  "ok": true, "dryRun": false, "undoStep": "AI: Create box brush",
  "result": { "brush": "brush:1042" },
  "changes": { "created": ["brush:1042"], "modified": ["layer:3"], "removed": [] },
  "selection": { "mode": "objects", "count": 1, "ids": ["brush:1042"], "truncated": false },
  "issuesIntroduced": [],
  "warnings": [],
  "grid": 16
}
```

Change lists are capped at 500 ids each with `"truncated": true` plus counts, so bulk edits cannot flood the context.

### 6.4 Selection-independence (X7) — "select, act, restore"

Almost every `Map_*` mutator operates on the **current selection** (`translateSelection`,
`setEntityProperty`, `setBrushFaceAttributes`, `csgHollow`, `groupSelectedNodes`, …). `Targets.h` therefore provides:

```cpp
Result<void> withTargets(CallContext& ctx, const TargetSpec& spec,
                         const std::function<Result<void>(const ResolvedTargets&)>& fn,
                         SelectionAfter after = SelectionAfter::Restore);
```

Inside the call transaction it saves the selection (nodes or faces), selects the targets (`deselectAll`
+ `selectNodes`/`selectBrushFaces`), runs `fn`, and restores the saved selection, dropping removed nodes. The
exceptions are tools whose editor counterpart leaves new objects selected: create, duplicate, paste, import,
and group. They use `SelectionAfter::Result`. The selection commands are part of the same undo step. With no
ids given, it uses the current selection and returns `NO_SELECTION` if it is empty. Targets that are not
editable (`EditorContext::selectable()` false: hidden, locked layer, or inside a closed group) fail with
`OBJECT_NOT_EDITABLE`. The hint names the fix: `layer_set_state` unlock/show, or `group_open`.

### 6.5 Non-map mutations

`Mutation::External` tools (save, preferences, grid, camera, compile, game path) are not undoable.
They get no transaction. They must honor `ctx.dryRun()` by validating and describing
(`"wouldDo": "overwrite /maps/a.map"`) without side effects. `Mutation::None` tools are read-only.

### 6.6 Required pre-fixes in TbMdlLib (E1.18)

- `CommandProcessor::executeAndStoreCommand` clears the redo stack even inside a transaction. A dry run
  or a failed call would then silently destroy the human's redo history. Move `m_redoStack.clear()` to the
  point where a command or transaction is stored on the top-level undo stack (`pushToUndoStack`), and add
  tests in `tst_CommandProcessor.cpp`.
- `Map::canRedoCommand()` returns `undoCommandName() != nullptr` (a bug). Fix it to use `redoCommandName()`
  (needed by `history_get`/`redo`, E1.23).

---

## 7. Tool definition DSL, errors, pagination, naming

### 7.1 Naming

- Tool names are the spec's names verbatim: `snake_case`, `domain_verb[_object]`, regex `^[a-z][a-z0-9_]{0,63}$`.
- `title` is short Title Case ("Create Box Brush"). It is used for the undo name `AI: <title>`, the status bar,
  and the call log.
- Common argument names: `ids` (objects), `faces`, `document`, `dryRun`, `cursor`, `limit`, `fields`, `detail`.
  Coordinates are `position`/`min`/`max`/`center`/`vector`, angles are `angle`/`angles` (degrees), and every length
  is in map units.
- Resource URIs: `trenchbroom://editor/status`, `trenchbroom://documents/{doc}/info|summary|selection|issues`,
  `trenchbroom://games/{game}/config|entity-definitions|materials`, `trenchbroom://compile/{run}/log`,
  `trenchbroom://console`, `trenchbroom://manual/{section}`, `trenchbroom://guide`.
- Prompt names: `blockout_level`, `populate_level`, `lighting_pass`, `texture_pass`, `fix_all_issues`,
  `compile_and_debug`, `explain_map`, `explain_entity`, `cleanup_map`.

### 7.2 Schema builder (`Schema.h`)

A tiny value-type DSL builds a `SchemaNode` tree. It serves both as the JSON Schema published in
`tools/list` (`toJsonSchema()`) and as the validator/decoder for incoming arguments (`validate(Json) ->
Result<Json, SchemaErrors>`, which fills in defaults). There is no generic JSON-Schema validator, because we
only accept what we declare.

```cpp
using namespace tb::mcp::schema;

void registerGeometryTools(ToolRegistry& r)
{
  r.add(ToolDef{"brush_create_box"}
    .title("Create Box Brush")
    .description("Creates a cuboid brush spanning min..max (map units, Z up). "
                 "Goes to the current layer or open group. Example: "
                 "{\"min\":[0,0,0],\"max\":[256,256,16],\"material\":\"base_floor\"}")
    .input(object({
      field("min", vec3()).required().describe("Minimum corner"),
      field("max", vec3()).required().describe("Maximum corner; each component > min"),
      field("material", string()).describe("Material name; default: current material"),
    }))
    .output(object({ field("brush", objectId({ObjectKind::Brush})) }))
    .mutation(Mutation::Map)          // adds `dryRun` + `document` automatically
    .handler(createBox));
}

ToolResult createBox(CallContext& ctx, const Args& a)
{
  const auto box = vm::bbox3d{a.get<vm::vec3d>("min"), a.get<vm::vec3d>("max")};
  ...
  return ToolResult{Json{{"brush", ctx.ids().format(*brushNode)}}};
}
```

Primitives: `boolean() integer() number() string() enumOf({...}) array(T) object({...}) oneOf({...})
vec3() vec2() box() color() objectId(kinds) faceRef() documentId()`, with modifiers `.required()
.defaultValue(j) .min(x) .max(x) .minItems(n) .pattern(re) .describe(s)`. `ToolDef` also sets the MCP
annotations: `readOnlyHint` (Mutation::None), `destructiveHint` (delete/close/overwrite tools),
`idempotentHint`, and `openWorldHint` (compile/engine/file tools). `Args::get<T>` uses `JsonVm.h`
converters. A handler never sees an unvalidated value.

Standard injected parameters:
- `Mutation::Map`/`External`: `document?: string`, `dryRun?: boolean = false`.
- List tools (`.paginated()`): `cursor?: string`, `limit?: integer = 100 (1..1000)`, `fields?: string[]`,
  `detail?: "summary"|"full" = "summary"`.

### 7.3 Errors (`Errors.h`, X9)

```cpp
struct ToolError {
  ErrorCode code;                     // serialized as UPPER_SNAKE string
  std::string message;                // what went wrong, in one sentence
  std::vector<std::string> objectIds; // involved objects
  std::string hint;                   // concrete next step, e.g. tool name + argument
  Json details = Json::object();      // e.g. schema error path
};
using ToolResult = Result<Json, ToolError>;  // kdl::result like the rest of the codebase
```

Codes: `INVALID_ARGUMENT`, `OBJECT_NOT_FOUND`, `WRONG_OBJECT_KIND`, `OBJECT_NOT_EDITABLE`,
`NO_SELECTION`, `NO_DOCUMENT`, `DOCUMENT_NOT_FOUND`, `INVALID_GEOMETRY`, `OUT_OF_WORLD_BOUNDS`,
`OPERATION_FAILED`, `TRANSACTION_ACTIVE`, `NO_TRANSACTION`, `BUSY_TIMEOUT`, `CANCELLED`,
`UNSAVED_CHANGES`, `FILE_EXISTS`, `IO_ERROR`, `UNSUPPORTED` (game/format), `UNSUPPORTED_IN_HOST`,
`DRY_RUN_UNSUPPORTED`, `INTERNAL_ERROR`.

Mapping to MCP:
- Tool failures, **including argument validation failures**, are returned as `CallToolResult` with `isError: true`,
  `structuredContent: {"ok":false,"error":{...}}`, and a text block such as
  `"INVALID_GEOMETRY: brush:12 would become non-convex. Hint: use a smaller offset."`. This follows the
  2025-11-25 guidance that input errors are tool errors, so the model can self-correct.
- JSON-RPC errors only for protocol faults: `-32700` parse, `-32600` invalid request, `-32601` unknown method,
  `-32602` unknown tool / bad `params` shape, `-32603` internal, `-32002` resource not found.
- Warnings (X14: unknown classname, property not in the definition, missing material) never fail a call. They
  go to `warnings: [{code, message, objectIds}]`.

### 7.4 Pagination and fields (X10)

- `cursor` is opaque: base64 of `{"o":<offset>,"m":<modificationCount>}`. If the document's
  `modificationCount` changed since the cursor was issued, the page is still served, with `"stale": true`.
- A list response is `{"items":[...], "total":N, "nextCursor":"..."|null}`.
- `fields` selects top-level keys and dotted paths (`"faces.material"`). `detail:"summary"` returns the
  documented compact shape per tool. `tools/list` itself returns all tools on one page (about 200 tools), while
  `resources/list` pages at 100.

---

## 8. Session log and observability

`CallLog` records every call: `{seq, time, session, client, tool, argsDigest, durationMs, ok,
errorCode, undoStep, changes counts}`. Arguments are kept in full up to 4 KB, then truncated. It is a ring
buffer of 5,000 entries, used by `session_log`. Sinks: (1) the document console through the document logger,
at info level, as `[AI] brush_create_box ok 3 ms (+1 brush)`; (2) when `McpLogToFile` is set, JSONL at
`<userDataDirectory>/mcp-logs/<yyyyMMdd-HHmmss>-<pid>.jsonl`, rotated at 10 MB. The protocol trace
(raw JSON-RPC) is logged only at debug level.

---

## 9. Testing strategy

### 9.1 `TbMcpLibTest` (headless, no Qt)

| Test file | Covers |
|---|---|
| `tst_JsonRpc.cpp` | parse/serialize, ids (string/number), batch gating, error codes |
| `tst_McpServer.cpp` | initialize/version negotiation, capability gating, ping, `notifications/initialized` ordering, cancellation, progress |
| `tst_HttpParser.cpp`, `tst_StreamableHttp.cpp`, `tst_SseParser.cpp` | split packets, limits, 202/400/404/403, sessions, SSE framing, Origin/Host checks — over a `FakeHttpConnection` |
| `tst_Schema.cpp` | builder → JSON Schema golden output; validation errors with paths; defaults |
| `tst_ToolRegistry.cpp`, `tst_ResourceRegistry.cpp`, `tst_PromptRegistry.cpp` | listing, paging, dispatch, subscriptions |
| `tst_ObjectIds.cpp` | id format/parse; delete→undo→same id; redo; linked-group edit keeps ids (aliasing); reload invalidation, layer/group remap |
| `tst_CallRunner.cpp` | one undo step named `AI: …`; rollback on failure leaves `modificationCount` and the undo stack unchanged; dry run leaves no trace, **redo stack preserved**; explicit transactions and nesting; busy gate with `FakeHost` + `FakeScheduler`; timeout |
| `tst_ChangeCollector.cpp` | created/modified/removed reduction, introduced issues |
| `tst_<Domain>Tools.cpp` | one test case per tool file, one `SECTION` per tool (AGENTS.md: one test case per unit, sections per function): happy path, invalid input, dry run, explicit ids vs selection |

Fixture (`TbMcpTestUtilsLib`):

```cpp
class McpToolFixture {       // wraps ui::MapDocumentFixture + FakeHost + FakeScheduler + registries
public:
  ui::MapDocument& create(mdl::MapFixtureConfig = mdl::QuakeFixtureConfig);
  ui::MapDocument& load(const std::filesystem::path&, mdl::MapFixtureConfig = {});
  Json call(std::string_view tool, Json args);              // returns structuredContent, CHECKs !isError
  ToolError callExpectingError(std::string_view tool, Json args);
  Json rpc(Json request);                                   // raw JSON-RPC through McpServer
  mdl::Node* node(std::string_view id);                     // resolve for assertions
};
```

It uses the existing `lib/TbMdlLib/test-utils` helpers (`MapFixture`, `QuakeFixtureConfig`,
`Quake2FixtureConfig`, `TestFactory.h`, `Matchers.h`) and `lib/TbAppLib/test-utils/MapDocumentFixture`.
Fixture maps live in `lib/TbMcpLib/test/fixture/test/mcp/`. Scenario tests (E10.4) are scripted
sequences in `tst_Scenarios.cpp` (S1–S3, S7 without compile) that run in CI.

### 9.2 `TbUiLibTest` (Qt, existing `RunAllTests.cpp` QApplication)

- `tst_McpTcpTransport.cpp`: listen on port 0, drive it with `QTcpSocket`/`QNetworkAccessManager`, wait with `QTest::qWaitFor`.
- `tst_QtMcpHost.cpp`: with a `MapWindow` (as `tst_MapWindow.cpp` does): document listing, busy detection (simulate `ToolBox` drag through `startMouseDrag`), `prepareForAgentEdit`, and action enumeration.
- `tst_McpServerController.cpp`: preference-driven start/stop, discovery file lifecycle.

### 9.3 Other

- `app/TrenchBroomMcp` holds no logic beyond wiring; its parsers are covered in `TbMcpLibTest`.
- Coverage: run with `-DTB_ENABLE_GCOV=1` per AGENTS.md; every tool file must cover its error branches.
- Build and test commands: `cmake --build <build> --target TbMcpLibTest && ctest --test-dir <build>/lib/TbMcpLib/test -j`.

---

## 10. Tool source layout (one file per domain)

| File (`src/tools/`) | Tools | Epic |
|---|---|---|
| `SessionTools.cpp` | `editor_status`, `document_list`, `document_activate`, `session_log` | E1 |
| `HistoryTools.cpp` | `undo`, `redo`, `history_get`, `transaction_begin/commit/rollback` | E1 |
| `DocumentTools.cpp` | `document_new/open/save/save_as/revert/close/recent`, `document_export_map/obj`, `autosave_list`, `map_files_list` | E2 |
| `GameTools.cpp` | `game_list`, `game_info`, `game_set_path`, `mods_*`, `entity_definitions_*`, `materials_collections_*`, `materials_reload`, `soft_bounds_*` | E2 |
| `SceneTools.cpp` | `map_summary`, `map_tree`, `object_get`, `objects_find`, `objects_at_point`, `ray_pick`, `space_check`, `map_plan_view`, `map_text_get`, `map_stats` | E3 |
| `SelectionTools.cpp` | `selection_*`, `select_*` | E3 |
| `GeometryTools.cpp` | `brush_create_*`, `room_create`, `opening_cut`, `brush_clip`, `face_extrude*`, `vertices_*`, `vertex_add`, `csg_*` | E4 |
| `TransformTools.cpp` | `objects_move/rotate/scale/shear/flip/duplicate/array/delete`, `command_repeat*` | E4 |
| `EntityTools.cpp` | all `entity_*` | E5 |
| `MaterialTools.cpp` | `materials_list`, `material_*`, `face_attributes_*`, `uv_*`, `locks_*`, `tags_list`, `tag_*` | E6 |
| `OrganizationTools.cpp` | `layers_list`, `layer_*`, `objects_move_to_layer`, `group_*`, `groups_merge`, `linked_group_*`, `visibility_set` | E7 |
| `ClipboardTools.cpp` | `clipboard_*`, `map_import` (prefabs in E11) | E7 |
| `ValidationTools.cpp` | `issues_list`, `issue_*`, `validators_*`, `map_check` | E8 |
| `CompileTools.cpp` | `compile_*`, `engine_*`, `pointfile_*`, `portalfile_*` | E8 |
| `ViewTools.cpp` | `camera_*`, `view_*`, `grid_get/set` | E4 (grid), E9 |
| `ActionTools.cpp` | `actions_list`, `action_invoke` | E9 |
| `PreferenceTools.cpp` | `preferences_get/set` | E9 |
| `KnowledgeTools.cpp` | `manual_search`, `manual_section` | E9 |
| `Resources.cpp`, `Prompts.cpp` | all resources (§7.1) and prompts | E3/E8/E10 |

Shared building blocks go in `src/tools/ToolUtils.{h,cpp}`: object serialization (`serializeNode(node,
fields, detail)`), face serialization, game-aware validation helpers (X14), and the `room_create`/`array` math. They
must not grow into a second tool file. `RegisterAll.cpp` calls every `register*Tools`, and `McpServerController`
calls `registerAll(registry)`.

Notes on specific domains:
- **Geometry creation** uses `mdl::BrushBuilder` and `mdl::addNodes(map, {{parentForNodes(map), nodes}})`. Shapes
  call `ui::DrawShapeToolExtension::createBrushes(bounds, DrawShapeToolParameters)` from TbAppLib (the exact code the
  Shape tool uses), with the parameters built from tool arguments.
- **Transforms** go through `withTargets` + `translateSelection`/`rotateSelection`/`scaleSelection`/
  `shearSelection`/`flipSelection` so that texture lock, entity angle updates, and repeat stack semantics match the editor.
- **ActionTools**: `ActionHost` enumerates `ActionManager::visitMainMenu`, `visitMapViewActions`, and
  `MapDocumentActionCache` tag/entity actions. The path is the action's preference path. `enabled`/`checked` are evaluated
  with an `ActionExecutionContext` for the target window. Dialog-opening actions come from a static allow-list in
  `QtMcpHost`. It is checked by the E9.10 coverage test, and each entry points to the matching semantic tool.

---

## 11. Implementation order

Each epic ends buildable, with tests passing and clang-format applied (TASKS.md rules). Commits within
an epic follow the listed order, so each commit builds and tests on its own.

**E1 — foundation (MVP)**
1. E1.2: CMake for `TbMcpLib` + `TbMcpLibTest` + `TbMcpTestUtilsLib` + README; nlohmann dependency (§2).
2. E1.3–E1.4: `Json`, `JsonRpc`, `ProtocolVersion`, `McpServer` lifecycle (transport-neutral), tests.
3. E1.20, E1.5, E1.19: `Errors`, `Schema`, `Args`, `ToolRegistry`, `Pagination`; tests.
4. E1.6–E1.7: `ResourceRegistry`, `PromptRegistry` (empty content), tests.
5. mdl pre-changes: `Node::runtimeId()`, `CommandProcessor::transactionDepth()`, the redo-stack fix, the `canRedoCommand`
   fix, `LoggingHub::targetLogger()`, each with tests in their own libraries.
6. E1.15: `ObjectIds`/`IdRegistry`; E1.17: `ChangeCollector`; E1.16/E1.18: `CallRunner`, `Targets`; E1.21 document
   targeting through `McpHost`; `FakeHost`/`McpToolFixture`.
7. E1.22–E1.24: `SessionTools`, `HistoryTools`, `CallLog`.
8. E1.8/E1.10: `HttpParser`, `StreamableHttp`, `SseParser` (core), then `McpTcpTransport`, `QtScheduler` (TbUiLib).
9. E1.11–E1.14: `McpServerController`, `QtMcpHost` (documents + busy state), preferences + pane, status indicator,
   log file, `--mcp-server`.
10. E1.9: `app/TrenchBroomMcp`. Done: Claude Code connects over HTTP and stdio.

**E2** DocumentTools + GameTools (`DocumentHost`, `PreferenceHost` for game paths), document/game resources,
progress for open/reload. **E3** SceneTools, SelectionTools, subscribable status/summary/selection resources.
**E4** GeometryTools + TransformTools + grid/locks. **E5** EntityTools + the entity-definitions resource.
**E6** MaterialTools + the materials resource. **E7** OrganizationTools + ClipboardTools (`map_import` parses the
file with `mdl::MapReader` into nodes, filters them, converts the format, and adds them through the paste path).
**E8** ValidationTools + CompileTools (`CompileHost` over `CompilationRunner`; tests use the existing `CmdTool`
stub like `tst_CompilationRunner.cpp`). **E9** ViewTools, `McpSnapshotRenderer`, ActionTools, PreferenceTools,
KnowledgeTools, coverage check. **E10** agent guide text, descriptions review, prompts, scenario tests, manual section.
**E11** `HeadlessHost` in TbMcpLib (no Qt; `MapDocument` instances owned by the host) plus a
`--headless-mcp` stdio mode in `TrenchBroomMcp` that links the core directly. Tools need no changes, which is the
payoff of the host seam.

---

## 12. Non-negotiable rules for implementation agents

1. No Qt includes in `lib/TbMcpLib`. No MCP protocol logic in `lib/TbUiLib`.
2. Map changes happen only through `mdl::` free functions/commands inside `CallRunner`'s transaction. Never
   mutate nodes directly.
3. Every tool: a description with an example, a declared input schema, a declared output schema,
   tests covering success, invalid input, and dry run (if modifying), plus explicit ids and selection (if targeting).
4. Never hold `mdl::Node*` across calls. Store ids and resolve through `IdRegistry` every time.
5. Handlers never block, never open dialogs, and never call `QApplication::processEvents`.
6. Destructive intent is explicit (`overwrite`, `discard`/`save`) and never prompts (PRD 7.1).

---

## 13. Implementation notes (E1, as built)

These notes record where the E1 implementation refines or deviates from the sections above.
They are binding for later epics in the same way as the rest of this document.

### 13.1 Core library

- **`Endpoint.h`** (new) is the transport-facing interface of `McpServer`:
  `post(sessionId?, body, shared_ptr<RequestStream>) -> PostResult{Accepted|Pending|BadRequest|SessionNotFound, newSessionId, body}`,
  `sessionProtocolVersion`, `openNotificationStream`, `deleteSession`. The server may complete a
  `RequestStream` before `post` returns; transports buffer output until `post` returns so that the
  `Mcp-Session-Id` header of a new session can be sent. The server keeps only weak references to
  streams; a dropped connection simply discards late output.
- **`ServerState.h`** (new) holds everything tools may need: host, scheduler, options, the
  registries, sessions, per-document state (`DocumentState`: `IdRegistry` + open
  `AgentTransaction`), the `CallRunner`, and the current `ServerActivity`. Handlers reach it through
  `CallContext::server()`. `McpServer::state()` exposes it to tests.
- **`McpHost`** (E1 subset): `applicationVersion`, `documents`, `busyState`,
  `prepareForAgentEdit`, `currentToolName`, `isCompileRunning`, plus two notifiers the host must fire:
  `documentWillCloseNotifier(MapDocument&)` (before a document is destroyed) and
  `documentsDidChangeNotifier` (open/close/focus). Sub-interfaces (§4.3) are still added by the epics
  that need them.
- **`ToolDef`** has, besides the fields in §7.2: `documentUse(None|Optional|Required)` (default:
  `Required` for `Mutation::Map`, `None` otherwise; `document` is injected for anything but `None`),
  and `transactional(bool)` (default true). Tools that manage the history themselves (`undo`, `redo`,
  `transaction_*`) are `Mutation::Map` + `transactional(false)`: they still go through the busy gate and
  get a change report, but must honor `dryRun` themselves. `CallContext::setUndoStep()` lets them name
  the undo step they created (e.g. `transaction_commit`).
- **Result shapes.** Read-only tools (`Mutation::None`) return the handler's JSON as
  `structuredContent` (plus `warnings` if any). Modifying tools return the envelope of §6.3;
  `External` tools omit `changes`, `selection` and `issuesIntroduced`. For 2025-03-26 sessions,
  `structuredContent`, `outputSchema` and titles are omitted; the JSON is in the text block.
  Published output schemas do not contain `additionalProperties: false`.
- **Argument validation** happens before a call is queued, so invalid calls fail immediately even while
  the human is busy. Unknown properties are rejected (with the list of allowed ones).
- **Warnings** produced by `prepareForAgentEdit` use the code `EDITOR_STATE_CHANGED`.
- **Undo step detection.** A call reports `undoStep` only if its transaction actually stored a command
  (observed through `transactionDoneNotifier`); calls that change nothing report `null`.
- **Dry run** computes the change report and introduced issues before the rollback. Linked-group
  propagation (which `Map::commitTransaction` performs) is not part of a dry run's report.
- **Object ids.** The world is always `world` and the default layer always `layer:default`
  (canonical, also accepted on input). A face id resolves to its brush; `resolveFace` in `Targets.h`
  returns the `BrushFaceHandle`.
- **Reload.** `IdRegistry` keeps, for every layer and group it has seen, its persistent id; after a
  reload, old layer/group ids resolve to the node with the same persistent id. Other ids assigned before
  the reload fail with a "document was reloaded" message.
- **`session_log`** lists newest first; cursors are tied to the last log sequence number, so a page
  requested after new calls were logged is marked `stale`.
- **Resources in E1:** `trenchbroom://editor/status` (subscribable; updated when documents open, close
  or change focus) and `trenchbroom://guide` (a first version of the agent guide; E10 replaces it).
- `CallLog` also provides `JsonlFileSink` (rotation at 10 MB with `.1`, `.2`, ... suffixes).

### 13.2 TbMdlLib / TbBaseLib / TbAppLib additions

- `Node::runtimeId()` (§5.2).
- `CommandProcessor::transactionDepth()` and `Map::transactionDepth()` (§6.2).
- `CommandProcessor::undoCommandNames()` / `redoCommandNames()` (most recent first), used by
  `history_get` and `undo`/`redo` (new, not in §6.6).
- The redo stack is now cleared only when a command or transaction reaches the top-level undo stack
  (`storeCommand` / `createAndStoreTransaction`), not when a command is executed inside a transaction
  (§6.6). Rolling back a transaction therefore keeps the redo history.
- `Map::canRedoCommand()` uses `redoCommandName()` (§6.6).
- `LoggingHub::targetLogger()` and a `MapDocument::targetLogger()` passthrough (§6.1).

### 13.3 Build

- An existing build tree must be configured with `-DFETCHCONTENT_UPDATES_DISCONNECTED=ON` before
  re-running CMake; otherwise the git update step of the patched dependencies (assimp, cpptrace, miniz)
  re-runs and re-applying the patch fails.

### 13.4 Transport and stdio bridge

- `StreamableHttpServer(Endpoint&, Config{bindAddress, accessToken, maxBodySize, path})` drives
  connections through `HttpConnection{write, close}`: `openConnection`, `feed`, `connectionClosed`,
  `sendKeepAlives`, `closeAllConnections`. `McpTcpTransport(Endpoint&, Config)` adapts it to
  `QTcpServer` (`listen(bind, port, token)`, `serverPort`, `errorString`, `closeAllConnections`,
  signal `connectionCountChanged`).
- Transport-level errors carry JSON-RPC bodies. In addition to §3.2: a wrong `Content-Type` gets `415`,
  a GET without an acceptable `Accept` gets `406`, a missing `Host` gets `400`. A wildcard bind
  (`0.0.0.0`) skips the Host check but still requires the token. A second GET or a DELETE ends the
  session's previous stream. SSE frames carry `event: message`; POST and GET streams of a session share
  one `id:` sequence.
- The bridge reads the discovery file from the same directory as `SystemPaths::userDataDirectory()`
  (`~/.TrenchBroom` on Linux, the application data location elsewhere; portable mode is not handled).
  After launching the editor it waits for a new or changed discovery file, so a stale file left by a
  crash is ignored. Lines on stdin that are not valid JSON are answered with `-32700` by the bridge
  itself. `app/CMakeLists.txt` adds `TrenchBroomMcp` before `TrenchBroom`, which copies the bridge next
  to the editor after building (into the bundle on macOS) and installs it.

### 13.5 Editor integration

- `McpServerController` (owned by `AppController`, destroyed first in its destructor) creates the
  server, `QtMcpHost`, `QtScheduler` and `McpTcpTransport` when enabled, and destroys them when
  disabled. It watches all `MCP/*` preferences: bind address, port or token changes restart the server;
  the busy timeout is applied with `McpServer::setOptions`; `MCP/Log to file` toggles the JSONL sink.
  `--mcp-server` calls `setForceEnabled(true)`. The discovery and log paths are derived from
  `EnvironmentConfig::userDataFolderPath` (the same directory as `SystemPaths::userDataDirectory()` in
  production, a temporary directory in tests). The discovery file is written atomically and removed on
  stop and quit; a killed editor leaves a stale file, which the bridge tolerates (§13.4).
- The console sink logs `[AI] <tool> ok 3 ms (+1 ~2 -0)` (or the error code) to the top map window's
  logger.
- `QtMcpHost::busyState`: `QApplication::activeModalWidget()` or the window's `ToolBox::dragging()`.
  `prepareForAgentEdit` deactivates the current tool when `selectionOwnedByTool()`, a node handle tool
  (vertex/edge/face) or the clip tool is active, because only some tools override `ownsSelection()`.
  `isCompileRunning` asks `MapWindow::compilationRunning()` (new, forwards to the compilation dialog).
- `MapWindowManager` got two signals that drive the host notifiers: `mapWindowWillClose(MapWindow*)`
  (emitted in `removeMapWindow`, before the window and its document are deleted) →
  `documentWillCloseNotifier`, and `mapWindowsDidChange()` (window created/closed, focus order changed,
  document created or loaded into an existing window) → `documentsDidChangeNotifier`. It also got a
  public `addMapWindow(MapWindow*)` (used by `createMapWindow`, and by tests because showing a window
  under the offscreen platform fails on OpenGL). Document handles `doc:<n>` follow window open order.
- `McpStatusIndicator` sits next to the update indicator in the status bar and has the **Stop agent**
  button (`McpServerController::stopAgents` → `McpServer::stopAgents` + close all connections).
- `McpPreferencePane` ("AI Agents (MCP)") uses a new icon `McpPreferences.svg`.
- A headless smoke test of the real editor works with `QT_QPA_PLATFORM=offscreen` and an isolated
  `HOME` whose `Preferences.json` sets `"updater/Ask for auto updates": false` (otherwise a modal update
  question blocks start-up before `--mcp-server` is processed).

### 13.6 Implementation notes (E2, as built)

**Host.** `McpHost` got `documentHost()` and `gameManager()`. `DocumentHost` (in `Host.h`) has
`documentToReplace()` (the document a new or loaded one replaces in single-window mode, else
nullopt), `createDocument(gameInfo, format)`, `loadDocument(gameInfo, format, path)` (format
`Unknown` = detect), `closeDocument(document)` (no questions, discards changes; the
`MapDocument` object must stay alive until control returns to the event loop) and
`recentDocuments()`. Create/load return `OpenedDocument{DocumentInfo, messages}` with the
warnings and errors logged while loading. There is no `PreferenceHost` yet: `game_set_path` uses
`setPref` on the game's `gamePathPreference` directly (Qt-free; open documents react through
their preference observer). Game and format detection (`readMapHeader`) happens in the core.

- `QtMcpHost` implements `DocumentHost`. New documents are built with
  `MapDocument::createDocument/loadDocument` and shown with `MapWindowManager::createMapWindow`
  (now public, as is `shouldCreateWindowForDocument`); in single-window mode the top window's
  document is recreated in place. `closeDocument` calls the new
  `MapWindow::closeWithoutConfirmation()`. Agent-created documents do not close the welcome
  window. Creating and loading are not covered by `TbUiLibTest` because showing a map window
  needs OpenGL, which the offscreen test platform lacks.
- `FakeHost` implements `DocumentHost` with its own task and resource managers and a
  `GameManager` with the games "Test", "Quake" and "Quake 2" (the real configurations from the
  fixture's `games/` folder, game paths in `test/mdl/Game/`). `singleWindow` simulates
  single-window mode; `recentDocumentList` is the recent list; closed documents stay alive.
  `McpToolFixture::call*` run pending scheduler tasks until an asynchronous call completes.

**Log capture.** `CapturingLogger`/`ScopedLogCapture` moved from `CallRunner.cpp` to
`LogCapture.h`, plus `LogMessage` and `collectCachedMessages(document)` (reads the messages a
document without a target logger has cached, and caches them again for its console).
`CallContext::setCapturedMessages` became `setLogCapture`; `loggedProblems()` returns the warnings
and errors logged for the target document during the call.

**Asynchronous tools (§4.2, E2.16).** `ToolDef::asyncHandler(fn)` with
`fn(CallContext&, const Args&, ToolCompletion)`; only for `Mutation::External` tools, which go
through the queue: the queue waits until the call completes. The handler continues in steps
scheduled with `CallContext::defer` and checks `CallContext::cancelled()` between them.
`notifications/cancelled` for a running asynchronous call sets that flag (cooperative); closing
the session or **Stop agent** abandons it with `CANCELLED` and drops its pending steps; if its
target document closes meanwhile, it fails with `DOCUMENT_NOT_FOUND`; an exception in a step
becomes `INTERNAL_ERROR`. `document_open`, `entity_definitions_reload` and `materials_reload`
are asynchronous: they emit progress, then load/reload in a deferred step, so a cancellation sent
meanwhile is honored. The load itself is synchronous and cannot be interrupted.

**Undo collation (X2 fix).** `CommandProcessor` collates a transaction with the previous one when
their first commands collate (e.g. two consecutive worldspawn changes within the collation
interval), which merged two agent calls into one undo step. `CallRunner` now disables collation
while it commits a call's transaction.

**Document tools** (`DocumentTools.cpp`):
- All paths are absolute (`INVALID_ARGUMENT` otherwise); the server does not create folders.
- `unsavedChanges: "error" | "save" | "discard"` (default `"error"` → `UNSAVED_CHANGES`) on
  `document_close`, `document_revert`, and on `document_new` / `document_open` when they replace a
  document (single-window mode). `"save"` fails for a never-saved document.
- `document_new` / `document_open` make the new document the session's active document.
  `document_new` reports the game's `initialMap` template for the format (or null).
- `document_open` reads game and format from the header comments; explicit `game` / `format`
  override them; a missing format is detected by the loader (`formatSource: "detected"`). A file
  that is already open is returned with `alreadyOpen: true` instead of opening it twice.
  `loadMessages` lists the warnings and errors logged while loading.
- `document_revert` reloads from disk (`idsInvalidated: true`); `document_close` refuses while a
  compilation runs.
- `document_save_as` accepts the document's own path without `overwrite`, and warns
  (`UNUSUAL_EXTENSION`) for paths not ending in `.map`. Exports refuse the document's own path.
- `map_files_list` matches a case-insensitive glob (default `*.map`), optionally recursive,
  sorted naturally. `autosave_list` lists `<map dir>/autosave/<name>.<n>.map`, newest first.

**Game tools** (`GameTools.cpp`):
- `game_info` without `game` describes the active document's game; smart tags are reported with
  their name, attributes and a textual definition.
- `mods_set`, `entity_definitions_set`, `materials_collections_set` and `soft_bounds_set` are
  `Mutation::Map` (worldspawn changes, one undo step each); the resulting reload problems become
  warnings (`LOAD_WARNING` / `LOAD_ERROR`). Unknown values are warnings (X14): `UNKNOWN_MOD`,
  `DEFAULT_MOD`, `FILE_NOT_FOUND`, `UNKNOWN_COLLECTION`, `BOUNDS_OUTSIDE_WORLD`.
- `materials_collections_set` takes `wads` (the ordered WAD list; WAD games only, else
  `UNSUPPORTED`) and/or `enabled` (enabled collection paths, any game).
  `entity_definitions_set` takes `type: "builtin" | "external"` and `path`.
- `soft_bounds_*` use `mode: "game" | "unlimited" | "custom"` with `bounds` for custom.
- Shared helpers (game lookup, ISO times, absolute path arguments, percent-encoding) are in
  `src/tools/ToolUtils.{h,cpp}`. `documentInfo()` (DocumentTools.h) and `gameConfigJson()`,
  `modsJson()`, `entityDefinitionsJson()`, `materialsJson()`, `softBoundsJson()`
  (GameTools.h) are shared with the resources.

**Resources.** `trenchbroom://documents/{doc}/info` (template; one entry per open document;
subscribers are notified when the document is saved, loaded, its modified flag flips, or mods,
entity definitions, materials or worldspawn change) and `trenchbroom://games/{game}/config`
(template; `{game}` is the percent-encoded game name; listed for the games of open documents;
notified by `game_set_path`). `ServerState` now creates a `DocumentState` for every open document
when the document list changes, so subscriptions work before any tool touched a document.

### 13.7 Implementation notes (E3, as built)

**Layout.** The spatial queries (`objects_at_point`, `ray_pick`, `space_check`, `map_plan_view`)
live in their own domain file `SpatialTools.cpp` (`registerSpatialTools`) instead of
`SceneTools.cpp`, which keeps `map_summary`, `map_tree`, `object_get`, `objects_find`,
`map_text_get` and `map_stats`. Shared object descriptions are in `src/tools/NodeJson.{h,cpp}`
(`nodeSummary`, `nodeState`, `faceJson`, `nodeLabel`, `nodeMaterials`, tag names, layer/group
ids): every list item that describes an object uses `nodeSummary`
(`{id, kind, label, bounds, layer, classname | name | materials, entity}`), every face
`faceJson`. `mapSummary()` (SceneTools.h) and `selectionDetails()` (SelectionTools.h) are
shared with the resources. The sample map `test/fixture/mcp/maps/two_rooms.map` (two rooms, a
corridor, a door, a trigger, a group and a custom layer) is the fixture for scene, spatial,
selection and resource tests; `SceneQuestions` answers the epic's acceptance questions with tool
calls only.

**Scene tools.**
- `map_tree` returns the flattened depth-first tree as a page; nodes at the depth limit carry
  `descendants` counts by kind instead of children. `kinds` filters items but containers are
  still traversed.
- `object_get` takes up to 50 object or face ids (default `detail: "full"`); one unknown id
  fails the whole call with all unknown ids listed. Point vs brush entity is decided by whether
  the entity has children. The layer color is not reported.
- `objects_find` filters are AND-combined; globs (`*`, `?`) are case-insensitive. An unknown tag
  name is a warning (`UNKNOWN_TAG`), not an error. Pages carry `counts` by kind for the whole
  match set.
- `map_text_get` pages by lines (`startLine`, `maxLines` ≤ 5000); a layer id stands for its
  contents. Line numbers match the file on disk only right after loading or saving.

**Spatial tools.**
- `ray_pick` casts rays with its own loop over the world octree and the editor's face and
  entity hit tests instead of `mdl::pick`, because `mdl::pick` always applies the editor
  context and cannot include hidden objects. Brush faces are hit from the front only, so a ray
  that starts inside a brush passes through it (as in the editor). `from: <id>` starts at the
  object's bounds center and ignores the object and its members ("what is under this
  entity?"). No hit is `hit: null`, not an error. Entity hits have no normal.
- `Brush::intersects(bbox)` compares bounds only, so `space_check` uses a private exact
  separating-axis test (face planes, box axes, edge × axis) on the box shrunk by 0.01; touching
  surfaces do not overlap. With `solidOnly` (default) brushes of `trigger_*` entities are
  ignored. Floor and ceiling come from five vertical rays (center and inset corners);
  `supportedCorners` counts corners with a surface within 1 unit.
- `map_plan_view` (text form only; the image form is E9) classifies cells by area at the given
  height: `#` solid (world, `func_group`, `func_detail*`), `+` other brush entity, `t`
  trigger, `.` open with a floor within `floorDepth` (1024) below the cell center, space for
  void. The grid is aligned to multiples of `cellSize`; entity chars `P M I E L` in that
  priority. Patches only count as floor.

**Selection tools.** All tools that change the selection are `Mutation::Map` (selection changes
are undoable editor commands), so each call is one undo step `AI: <title>` and supports dry run;
`selection_get` is read-only and paginated. `selection_set` refuses to mix objects and faces
(`INVALID_ARGUMENT`), world/layer ids (`WRONG_OBJECT_KIND`, hint: `select_by` layers) and
non-selectable objects (`OBJECT_NOT_EDITABLE`, not checked in remove mode). `select_by` takes
exactly one criterion; no match is `count: 0` plus a `NO_MATCH` warning. `select_faces_of`
without `ids` or `face` uses the selected brushes (X7); `coplanar` defaults to true and uses
`collectConnectedCoplanarFaces`. Preconditions of the `Map_Selection` functions are checked
before calling them. Known gaps: `selection_get` cursors are keyed to the modification count,
so a selection-only change does not mark a page `stale`; a failed "tall" selector brush is only
logged by the editor.

**Resources and notifications.**
- New templates `trenchbroom://documents/{doc}/summary` (= `map_summary`) and
  `trenchbroom://documents/{doc}/selection` (= `selectionDetails`, at most 100 items), listed
  once per open document.
- `DocumentState` reports `DocumentAspect::{Info, Summary, Selection, Status}` changes.
  Summary: nodes added/removed/changed, visibility, locking, current layer, grid, entity
  definitions, reload. Selection: selection changes and changes of selected nodes. The editor
  status is updated on info and selection changes, grid, tool changes, lock preferences
  (`AlignmentLock`, `UvLock`) and agent transactions opening or closing.
- Info notifications and document open/close stay immediate. All other updates go through
  `ServerState::scheduleResourceUpdate` / `scheduleDocumentUpdate`: they are coalesced into one
  `notifications/resources/updated` per resource and scheduler turn, and nothing is recorded
  while no session has subscriptions (the hooks run on every map change, e.g. during drags).
- `McpHost::currentToolDidChangeNotifier(MapDocument&)` is new; `QtMcpHost` fires it from each
  map window's `ToolBox` `toolActivatedNotifier` / `toolDeactivatedNotifier`.

### 13.8 Implementation notes (E4, as built)

**Layout.** The geometry tools are split over two domain files instead of one
`GeometryTools.cpp`: `GeometryTools.cpp` (`brush_create_box/shape/hull`, `room_create`,
`opening_cut`) and `BrushEditTools.cpp` (`brush_clip`, `face_extrude*`, `vertices_*`,
`vertex_add`, `csg_*`). `TransformTools.cpp` holds `objects_*` and `command_repeat*`,
`ViewTools.cpp` `grid_get/set`, and `MaterialTools.cpp` `locks_get/set` (E6 adds the material
tools there). Shared helpers are in `src/tools/GeometryUtils.{h,cpp}`: `brushBuilder` (game face
defaults), `materialArgument` (`UNKNOWN_MATERIAL` warning), `checkBox`,
`checkInsideWorldBounds`, `geometryError` / `geometryOperationFailed` (E4.16),
`addBrushes`, `nodeSummaries`, `warnNonIntegerVertices` (`NON_INTEGER_VERTICES`, S7),
`ScopedLockOverride` (per-call `alignmentLock` / `uvLock` overrides) and `intersectsInterior`
(moved here from `SpatialTools.cpp`, now shared with `opening_cut`).

**Validity errors (E4.16).** Degenerate or non-convex results are `INVALID_GEOMETRY`, results
that reach or leave the world bounds `OUT_OF_WORLD_BOUNDS`; both name the involved ids and
the editor's logged message (`details.editorMessages`). Brush geometry is clipped to the
world bounds by `mdl::Brush`, so touching the world bounds counts as out of bounds. Vertex,
edge and face moves pre-check each brush (`canTransformVertices` etc.) to name the offending
brush. The editor logs nothing when a brush transform fails; the transform tools compute
the transformed bounds themselves to report `OUT_OF_WORLD_BOUNDS`.

**TbMdlLib.** `csgHollow(Map&, std::optional<double> thickness = std::nullopt)`: the wall
thickness defaults to the grid size; a thickness ≤ 0 fails.

**Creation.**
- Creation tools select their result (as the editor does). `brush_create_shape` calls the
  editor's `ui::DrawShapeTool*Extension` classes; `material` is applied to the faces of the
  returned brushes instead of changing the current material. Parameters that do not apply
  to the chosen shape are warned about (`IGNORED_ARGUMENT`), so their defaults are applied
  in code, not in the schema. The arch axis defaults to `x` (upright arch). Hollow cylinder
  and arch thicknesses are validated (the editor would silently build solid wedges);
  a step height ≥ the box height only warns (`SINGLE_STEP`). The editor preference that
  groups shape brushes automatically is not read; the `group` argument covers it.
- `brush_create_hull` explains degenerate point sets (coincident, collinear, coplanar)
  and warns about unused points (`POINTS_INSIDE_HULL`).
- `room_create`: floor and ceiling span the outer footprint, the west/east walls the outer
  depth, and the south/north walls fit between them; the result names each brush by role.
- `opening_cut` subtracts the opening from each target with `Brush::subtract`, re-applies
  the wall's own face attributes (subtract copies the cutter's attributes to coplanar
  faces) and uses `material` or the wall's most used material inside the opening. Any
  invalid fragment fails the whole call (the editor's `csgSubtract` drops it). Without ids
  it cuts every selectable brush the opening overlaps.

**Transforms.**
- Rotate and flip default to the exact bounds center (the editor uses its grid reference
  point). `objects_rotate` sets the world's `updateAnglePropertyAfterTransform` for the call
  (`updateEntityAngles`, default true) and restores it.
- `objects_array` `count` is the total number of instances including the originals
  (≤ 1024); circle arrays rotate the copies around the center (`rotate`, default true), so
  they keep facing it, and `rise` offsets each instance along the axis (spiral stairs). The
  array is left selected; the call is one undo step and one repeatable entry.
- `command_repeat` is `transactional(false)`: the repeat stack refuses to repeat while a map
  transaction is open, so the tool opens its own `LongRunning` command-processor transaction
  named `AI: Repeat Last Commands`, handles dry run itself (rollback; the change report of a
  dry run is empty, the result lists the selection), and fails with `TRANSACTION_ACTIVE` inside
  an agent transaction. Each agent call is one repeatable entry. Selection changes made by
  tools happen inside the call transaction and therefore do not start a new repeat
  recording (would need a CallRunner/TbMdlLib hook).
- `grid_set` / `locks_set` are `Mutation::External` (grid on the map, locks via the
  `AlignmentLock` / `UvLock` preferences, which `MapDocument` applies to the editor context).

**Brush editing.**
- `brush_clip`: the plane normal is `cross(p1-p0, p2-p0)`, with 2 points `cross(b-a, axis)`,
  with `face` the face normal; "front" is the side the normal points to.
- `face_extrude` groups faces by normal and extrudes each group; `face_extrude_new` reimplements
  the Extrude tool's split (outward / inward for negative distances) and stamp logic with mdl
  calls, since those functions are file-local to `ExtrudeTool.cpp`.
- `vertices_move` targets the selected brushes, or else every editable brush that has one of
  the handles; positions match within 0.01.
- Clip, extrude-to-new and all `csg_*` tools leave their results selected (editor behavior);
  the others restore the selection. Subtracting with cutters that touch nothing and
  intersecting disjoint brushes follow the editor (brushes are removed) and warn
  (`NOTHING_SUBTRACTED`, `EMPTY_INTERSECTION`). Other warnings: `NOTHING_CLIPPED`,
  `VERTICES_MERGED`, `SNAP_FAILED`, `NOT_HOLLOWED`.
- An explicit id of the wrong kind fails schema validation (`INVALID_ARGUMENT`);
  `WRONG_OBJECT_KIND` is returned for a selection of the wrong kinds.

**Tests.** `tst_GeometryTools`, `tst_BrushEditTools`, `tst_TransformTools`, `tst_ViewTools`,
`tst_MaterialTools`, and `tst_Scenarios` ("Scenario S7": 12 columns on a circle of radius 384
facing the center, a 20-step spiral staircase, each one undo step).

**Known gaps.** `csg_subtract` does not map fragments to the brush they came from;
`vertices_move` reports `hasRemainingVertices` only for vertex moves; the world bounds
error after `objects_duplicate` names copy ids that will not exist.
