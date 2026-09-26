# Connecting an AI Agent to TrenchBroom

TrenchBroom includes an MCP (Model Context Protocol) server. When it is on, an AI client such as Claude Code or Claude Desktop can read and edit the map open in the editor. Every agent edit appears live in the editor, and you can revert it with Undo.

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

The `TrenchBroomMcp` executable sits next to the TrenchBroom executable. It finds a running editor through the discovery file. If no editor is running, it launches one with `--mcp-server`.

```bash
claude mcp add --scope user trenchbroom -- /path/to/TrenchBroomMcp
```

The bridge launches TrenchBroom as soon as the client starts, even before any tool call. Add `--no-launch` if you only want to connect to an editor you opened yourself:

```bash
claude mcp add --scope user trenchbroom -- /path/to/TrenchBroomMcp --no-launch
```

With `--no-launch`, start TrenchBroom first, then reconnect with `/mcp` in Claude Code.

Bridge options:

| Option | Meaning |
|---|---|
| `--port N` | Connect to port N instead of reading the discovery file |
| `--no-launch` | Fail instead of launching TrenchBroom when it is not running |
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

## 4. Remote access

By default the server only accepts connections from the local machine. To connect from another machine:

1. Set **Bind address** under Preferences → AI Agents.
2. Set an **Access token** on the same page.
3. Clients must send the header `Authorization: Bearer <token>`.

## 5. Logs and troubleshooting

| Symptom | What to check |
|---|---|
| The editor started by the bridge does not answer | On the first start TrenchBroom asks whether to check for updates. The server starts only after you answer that question. |
| Client cannot connect | Is the server on? Check the status bar indicator. Is the port free? If it is taken, the status shows "MCP: port … in use". |
| The bridge starts a second editor | A stale `mcp-server.json` was left by a crashed editor. The bridge tolerates it, or you can delete the file. |
| A call waits and then times out | The agent waits while you drag, hold a modal dialog open, or have your own edit in progress. Finish the interaction. The timeout is **Busy wait timeout** under Preferences → AI Agents. |
| Need to see what the agent did | Agent calls appear in the editor console as `[AI] …`. Full JSONL logs are in `<user data folder>/mcp-logs/`. |
