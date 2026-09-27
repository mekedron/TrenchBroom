# TrenchBroom MCP Server — Documentation

Product documentation for an MCP (Model Context Protocol) server that lets AI agents fully operate the TrenchBroom level editor: create and open maps, build geometry, place entities and NPCs, apply materials, validate, compile and launch maps in the game engine.

| Document | Purpose |
|---|---|
| [OVERVIEW.md](OVERVIEW.md) | What the server does, highlights, getting started, and why this is a fork |
| [01-PRD.md](01-PRD.md) | Vision, goals, users, product concept, non-functional requirements, phases, risks |
| [02-editor-capabilities.md](02-editor-capabilities.md) | What TrenchBroom can do today — the baseline the server must cover, with gaps marked |
| [03-functional-spec.md](03-functional-spec.md) | The full list of MCP tools, resources and prompts, grouped by domain, with priorities and a coverage matrix |
| [04-scenarios.md](04-scenarios.md) | End-to-end agent scenarios with acceptance criteria |
| [05-technical-design.md](05-technical-design.md) | Architecture, transport, threading, tool machinery, testing and the upstream changes |
| [06-scenario-results.md](06-scenario-results.md) | Results of the scenarios run by an agent against the editor, and the gaps found |
| [CONNECTING.md](CONNECTING.md) | User guide: turn on the server, connect Claude Code, Claude Desktop or an IDE, the mapping skills, troubleshooting |
| [TASKS.md](TASKS.md) | Implementation epics and their status |
