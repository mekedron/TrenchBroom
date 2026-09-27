# Connecting an AI Agent to TrenchBroom

TrenchBroom includes an MCP (Model Context Protocol) server. When it is on, an AI client such as Claude Code or Claude Desktop can read and edit the map open in the editor. Every agent edit appears live in the editor, and you can revert it with Undo.

The pieces:

- **The MCP server** runs inside TrenchBroom. It is off until you turn it on (section 1).
- **`TrenchBroomMcp`**, the stdio bridge, sits next to the TrenchBroom executable: `trenchbroom` and `TrenchBroomMcp` in the same folder on Linux (`build/app/TrenchBroom/` in a build tree), `TrenchBroom.exe` and `TrenchBroomMcp.exe` on Windows, and `TrenchBroom.app/Contents/MacOS/` on macOS. Clients that start MCP servers as programs use it.
- **The mapping skills** (optional, Claude Code) teach the agent how to build good maps (section 5).

Before the agent builds maps, set the game paths under **Preferences → Games** and, to compile, the compile tool paths there as well. The agent can also do this itself (`game_set_path`, `compile_tools_set`).

## 1. Turn on the server

Use either of these:

- **Preferences → AI Agents → Enable MCP server.** The server starts right away and starts again every time you open TrenchBroom.
- **Command line:** `trenchbroom --mcp-server` turns it on for this run only.

Default endpoint: `http://127.0.0.1:47100/mcp`. You can change the port under Preferences → AI Agents.

While the server is running:

- The status bar shows the MCP indicator, including connected clients and what the agent is doing. **Stop agent** cancels whatever the agent is doing right now and disconnects it.
- The server writes a discovery file, `mcp-server.json`, to the user data folder. On Linux that is `~/.TrenchBroom`; on macOS `~/Library/Application Support/TrenchBroom`; on Windows `%APPDATA%\TrenchBroom`.

## 2. Connect Claude Code

### Option A — HTTP (TrenchBroom is already running)

```bash
claude mcp add --scope user --transport http trenchbroom http://127.0.0.1:47100/mcp
```

### Option B — stdio bridge (starts TrenchBroom if needed)

The `TrenchBroomMcp` executable sits next to the TrenchBroom executable. It finds a running editor through the discovery file. If no editor is running when the agent first needs it, it launches one with `--mcp-server`.

```bash
claude mcp add --scope user trenchbroom -- /path/to/TrenchBroomMcp
```

Starting the client does not open TrenchBroom: the bridge itself tells the client which tools, resources and prompts exist. It connects to TrenchBroom, or launches it, only on the first tool call or resource read. Add `--no-launch` if you only want to connect to an editor you opened yourself:

```bash
claude mcp add --scope user trenchbroom -- /path/to/TrenchBroomMcp --no-launch
```

With `--no-launch`, a tool call while TrenchBroom is not running fails with a message that asks you to start TrenchBroom with the MCP server turned on. Start it, and the next tool call connects; you do not need to reconnect the client. The bridge also reconnects by itself when you restart TrenchBroom.

Bridge options:

| Option | Meaning |
|---|---|
| `--port N` | Connect to port N instead of reading the discovery file |
| `--no-launch` | Fail tool calls instead of launching TrenchBroom when it is not running |
| `--editor PATH` | TrenchBroom executable to launch (default: the one next to the bridge) |

### Check the connection

Run `claude mcp list`, or type `/mcp` inside Claude Code. `trenchbroom` should be listed as connected. Then ask something like "What is the status of the TrenchBroom editor?" and the agent will call `editor_status`.

`--scope user` makes the server available in every project. Use `--scope project` instead to store it in the project's `.mcp.json` and share it with the team.

## 3. Connect Claude Desktop or another client

Add this to the client's MCP configuration (for Claude Desktop, `claude_desktop_config.json`):

```json
{
  "mcpServers": {
    "trenchbroom": {
      "command": "/path/to/TrenchBroomMcp"
    }
  }
}
```

A client that supports the Streamable HTTP transport can connect to `http://127.0.0.1:47100/mcp` directly.

### IDEs

Most IDE agents use the same two forms: a stdio server with the bridge as `command`, or an HTTP server with the URL.

- **VS Code** (`.vscode/mcp.json` in the workspace, or the user configuration):

  ```json
  {
    "servers": {
      "trenchbroom": { "type": "stdio", "command": "/path/to/TrenchBroomMcp" }
    }
  }
  ```

- **Cursor, Windsurf and other clients** with an `mcpServers` file: use the Claude Desktop form above, or `{"url": "http://127.0.0.1:47100/mcp"}` for HTTP.

Add `"args": ["--no-launch"]` to the stdio form if the bridge should never start TrenchBroom.

## 4. Remote access

By default the server only accepts connections from the local machine. To connect from another machine:

1. Set **Bind address** under Preferences → AI Agents.
2. Set an **Access token** on the same page.
3. Clients must send the header `Authorization: Bearer <token>`.

## 5. What the agent gets

- **Tools** for every editor feature: documents, geometry, entities, materials and UVs, layers and groups, validation, compiling, the user's views and every menu action, plus agent-only ones such as offscreen snapshots, room analysis and free-spot search. Every edit is one undo step named "AI: …".
- **The agent guide**, the resource `trenchbroom://guide`: conventions, player dimensions per game, the recommended workflow and common pitfalls. The server's instructions tell the agent to read it first.
- **Prompts**, ready-made tasks you can pick in the client: `blockout_level`, `populate_level`, `lighting_pass`, `texture_pass`, `fix_issues`, `compile_and_debug`, `explain_map`, `explain_entity` and `cleanup_map`. In Claude Code they are slash commands, e.g. `/mcp__trenchbroom__blockout_level`.
- **Mapping skills** for Claude Code, in this repository under `.claude/skills/`: `trenchbroom-mapping` (the working method for any game: proportions, texturing, geometry recipes, z-fighting, compiling) and `halflife-mapping` (Half-Life units, WADs, textures, entity recipes, compiling and launching). Claude Code loads them automatically when you work in this repository. To use them elsewhere, copy the two folders to `~/.claude/skills/` (all projects) or to the project's `.claude/skills/`.

Claude Code asks for permission before each new tool. To allow all TrenchBroom tools at once, add `"mcp__trenchbroom"` to `permissions.allow` in `.claude/settings.json` (or answer "always allow" when asked).

## 6. Logs and troubleshooting

| Symptom | What to check |
|---|---|
| The first tool call waits and then fails with "TrenchBroom did not start" | The bridge launches TrenchBroom on the first tool call. On its first start TrenchBroom asks whether to check for updates; the server starts only after you answer that question. Answer it and repeat the call. |
| Client cannot connect | Is the server on? Check the status bar indicator. Is the port free? If it is taken, the status shows "MCP: port … in use". |
| The bridge starts a second editor | A stale `mcp-server.json` was left by a crashed editor. The bridge tolerates it, or you can delete the file. |
| A call waits and then times out | The agent waits while you drag, hold a modal dialog open, or have your own edit in progress. Finish the interaction. The timeout is **Busy wait timeout** under Preferences → AI Agents. |
| A tool call fails with "TrenchBroom is not running" | The bridge runs with `--no-launch` and no editor has its server on. Start TrenchBroom with the server enabled and repeat the call. |
| The client lists tools, but TrenchBroom never opens | That is expected until the first tool call: the bridge answers the lists itself. If the first call fails, check `--editor` or that the bridge sits next to the TrenchBroom executable. |
| The tool list changes after the first call | The bridge and the running TrenchBroom come from different builds. Use the bridge that ships with the editor. |
| Snapshots fail or are black | Snapshots need OpenGL 2.1. They work on a normal desktop session and under the `offscreen` Qt platform with Mesa. |
| An agent transaction stays open after the client reconnected | Transactions belong to the MCP session that opened it. Close the old session (restart the client) or undo in the editor. |
| Need to see what the agent did | Agent calls appear in the editor console as `[AI] …`. Full JSONL logs are in `<user data folder>/mcp-logs/`. The agent can read the editor's console with `console_read`. |
| Agent edits need to be reverted | Use Undo in the editor: each agent call is one undo step named "AI: …" (Edit → Undo, or the history). |
