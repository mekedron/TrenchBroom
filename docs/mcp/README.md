# TrenchBroom MCP Server — Documentation

Product documentation for an MCP (Model Context Protocol) server that lets AI agents fully operate the TrenchBroom level editor: create and open maps, build geometry, place entities and NPCs, apply materials, validate, compile and launch maps in the game engine.

| Document | Purpose |
|---|---|
| [01-PRD.md](01-PRD.md) | Vision, goals, users, product concept, non-functional requirements, phases, risks |
| [02-editor-capabilities.md](02-editor-capabilities.md) | What TrenchBroom can do today — the baseline the server must cover, with gaps marked |
| [03-functional-spec.md](03-functional-spec.md) | The full list of MCP tools, resources and prompts, grouped by domain, with priorities and a coverage matrix |
| [04-scenarios.md](04-scenarios.md) | End-to-end agent scenarios with acceptance criteria |

These documents describe **what** the server must do. Technical design (architecture, transport, threading, API schemas) will follow in separate documents.
