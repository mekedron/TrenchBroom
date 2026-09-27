# TrenchBroom MCP Library

Contains the Qt-free core of TrenchBroom's [Model Context Protocol](https://modelcontextprotocol.io)
server, which lets AI agents inspect and edit maps. It links no `Qt6::` target, like
`TbAppLib`; the editor glue (TCP sockets, the Qt scheduler, preferences, the status bar
indicator) lives in `TbMcpUiLib` (`McpServerController`, `McpTcpTransport`, `QtMcpHost`,
...), and the stdio bridge is the `TrenchBroomMcp` executable in `app/`.

The design is described in `docs/mcp/05-technical-design.md`. How to turn the server on and
connect Claude Code, Claude Desktop or an IDE, the mapping skills and troubleshooting are in
`docs/mcp/CONNECTING.md`; the scenario results in `docs/mcp/06-scenario-results.md`.

For agents, the server offers the guide resource `trenchbroom://guide` (`Resources.cpp`), nine
prompts (`Prompts.cpp`) and a description with an example for every tool
(`tst_ToolCatalog` checks them).

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
- `BridgeSession`: the protocol logic of the stdio bridge that works without the editor.
  An offline `McpServer` (all registries, a host without editor) answers `initialize` and
  the list requests; it routes the other messages to the editor and builds the handshake
  that opens the editor session on the first call that needs it.

## Tools, resources and prompts

- `ToolRegistry` / `ToolDef`: declares a tool (name, title, description, input and output
  schema, mutation kind, handler). `Schema.h` is a small DSL that produces both the
  published JSON Schema and the argument validator.
- `ResourceRegistry`, `PromptRegistry`: static resources, URI templates, subscriptions,
  prompt templates.
- `Resources`: the static resources and the agent guide (`trenchbroom://guide`);
  `Prompts`: the task prompts (`blockout_level`, `fix_issues`, ...). Both name tools in
  backticks, and `tst_Prompts` checks that every named tool exists.
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
     `context.defer` (optionally delayed), check `context.cancelled()` between steps, and call
     the completion once (see `document_open`). Read-only tools may be asynchronous too; they
     start immediately and may run while other calls run (see `view_snapshot`).
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
- `ChangeCollector`: created / modified / removed ids and introduced issues of a call (editor validators and
  the MCP placement checks).
- `Pagination`: cursors, limits and field selection for list tools.
- `ListDetail`: the `detail` level of modifying calls (summary / ids / full); cuts long id lists in the change
  report and the tool's result (`truncateIdLists`), counts them per kind and describes them in `truncatedLists`;
  the session keeps the full lists for `result_list_get`.
- `CallLog`: the ring buffer behind `session_log` plus sinks (console, JSONL file).
- `Host`: the editor as seen by the server (`McpHost`, with the `DocumentHost` sub-interface
  that creates, loads and closes documents, the `CompileHost` sub-interface that runs
  compile profiles as `CompileJob`s and the `EngineHost` sub-interface that launches game
  engines). The editor implements it in `TbMcpUiLib` (`QtMcpHost`, `McpCompileHost`,
  `McpEngineHost`); tests use `FakeHost`, `FakeCompileHost` and `FakeEngineHost` from
  `TbMcpTestUtilsLib`. E14 adds the `ViewHost` (the user's views, their cameras and the maximized
  view), `ActionHost` (the editor's action registry) and `PreferenceHost` (action shortcuts and the MCP
  preferences) sub-interfaces and `manualPath()` (editor: `McpViewHost`, `McpActionHost`,
  `McpPreferenceHost`; tests: `FakeViewHost`, `FakeActionHost` or `FakeHost::actionHostOverride` for the
  real one, `FakePreferenceHost`, `FakeHost::manualFile`).
- `Snapshot` (`SnapshotRenderer`, `SnapshotRequest`), `AgentCamera`, `Image`: agent vision. The
  core resolves agent cameras and visibility options into a request; the host's renderer
  (`McpSnapshotRenderer` in `TbMcpUiLib`, `FakeSnapshotRenderer` in tests) draws it offscreen
  without touching the user's views. `Image` has PNG encoding, side-by-side composition and
  changed-pixel diffs.
- `CameraProjection` (`makeGlCamera`, `ImageProjection`): the one camera of the renderer and the core, pixel rays
  for `view_pick` and point projection; `Annotations`: labels, grids, compass and player drawn onto rendered
  images. Every snapshot gets a `snapshotId`; the `Session` keeps the cameras of the recent ones.
- `MapManifest`: the per-map manifest `<name>.mcp.json` (spaces, key points, notes, agent cameras), kept in memory
  for unsaved maps and written when the document is saved.
- `ConsoleBuffer`: the editor's console messages from editor start (filled by
  `McpConsoleHook` in `TbMcpUiLib`, by `FakeHost` in tests); `console_read` reads it, and
  every call result lists the warnings and errors logged while the call ran (`console`).
- `CompileRuns`: the compile runs (`run:<n>`) with their jobs and logs, owned by `ServerState`.
- `LogCapture`: records the warnings and errors the editor logs during a call or while a
  document loads.
- `src/tools/ToolUtils`: small helpers shared by the tool files (game lookup, paths, times).
- `tools/GeometryUtils` (public header, model helpers tested directly): helpers of the
  geometry, brush editing and transform tools (brush builder with game defaults, material
  argument, world bounds and validity errors, `intersectsInterior`, `castRay`, brush
  classification, non-integer vertex warnings, `ScopedLockOverride`).
- `tools/AssetUtils` (public header): the game file system and material images read on the
  CPU (the editor drops texture pixels after uploading them).
- `tools/MaterialKnowledge`, `tools/UvCheck` (public headers, tested directly): material
  profiles merged from knowledge notes, the game's smart tags, a scanned reference corpus,
  the current map and image analysis, each value with its source; the texturing checks of
  `uv_check` and of the warnings of the material and face tools. Corpus statistics and
  notes are stored per game and mod in `McpHost::knowledgeDirectory()`.
- `tools/EntityModelUtils` (public header, tested directly): entity models loaded from the
  game files when the editor has not loaded them yet, animations with real bounds, the
  property that selects the animation, and placement checks against the model bounds.
- `tools/SpaceAnalysis` (public header, tested directly): the voxel grid of empty space (automatic cell size,
  optional region), spaces and openings, free spots, the 2.5D walking plan and leak prediction behind `spaces_list`,
  `surroundings`, `free_spots`, `walkable_plan`.
- `tools/PlacementChecks` (public header, tested directly): z-fighting and the per-call placement tracker that adds
  the MCP checks (z-fighting, entities outside the hull, model placement, UV distortion) to `issuesIntroduced`;
  `issues_list` reports them with the editor validators' issues, `issue_fix` applies the editor's quick fixes
  and the MCP checks' fixes, and `validators_set` turns validators and checks off per document
  (`DocumentState::disabledValidators`).
- `tools/MapCheckTools`: `map_check`, the agent-oriented checks (entities in walls or floating, player start, entity
  links, missing materials, entities outside rooms) with a suggested fix per finding, built on `PlacementChecks`,
  `EntityModelUtils` and `SpaceAnalysis`.
- `tools/ViewTools`: `grid_*`, the user camera tools and `view_layout_set` over `ViewHost`; `view_options_*`
  set the view preferences and the document's `EditorContext` directly.
- `tools/ActionCatalog` (public header, tested directly): the MCP classification of every editor action (invoke /
  dialog / refuse, dialog kind, matching semantic tools) behind `actions_list` and `action_invoke`; the coverage test
  in `TbMcpUiLibTest` fails when upstream adds or renames an action. `ToolDef::keepsActiveTool()` makes a Map tool
  (`action_invoke`) skip `prepareForAgentEdit`.
- `tools/PreferenceCatalog`, `PreferenceTools`: when upstream adds a preference to `prefs/Preferences.h`, add an entry
  to `editorPreferences()` (`tst_PreferenceTools` fails until you do); host-only preferences come from
  `PreferenceHost`.
- `tools/Manual`, `KnowledgeTools`: the user manual from `McpHost::manualPath()` (the generated `manual/index.html`;
  fixture in `test/fixture/mcp/manual/`), `manual_search`, `manual_section` and the manual resources.
- `src/tools/EntityUtils`: helpers of the entity tools (definition lookup, property type
  descriptions, value validation with X14 warnings, flag lookup by name, `resolveEntities` /
  `withEntities` for tools that act on entities including worldspawn).
- `tools/CompileUtils`, `tools/CompileLog` (public headers, tested directly): compile presets
  per game family, tool path checks, profile JSON; analysis of compile logs (tasks, exit
  codes, errors, warnings, leaks, output files).
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
