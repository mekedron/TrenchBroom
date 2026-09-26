# Fork rules (TrenchBroom MCP server)

This repository is a fork of TrenchBroom that adds an MCP server for AI agents. It must stay easy to sync with upstream TrenchBroom. These rules add to AGENTS.md.

- **Minimal upstream footprint.** Put all MCP code in new files: `lib/TbMcpLib`, `Mcp*` files in other libraries, `app/TrenchBroomMcp`. Change original TrenchBroom files only when there is no other way — a critical bug fix, or a hook that cannot be added from outside — and keep each change as small as possible.
- **Tests go into new files.** Never add tests to existing upstream test files; use new test files, preferably in `TbMcpLibTest`.
- **Document every upstream change.** Each change to an original file is listed with its reason in the "Upstream changes" section of `docs/mcp/05-technical-design.md`.
- **License headers.** New files use `Copyright (C) <year> Nikita Rabykin` in the GPL header; modified files keep their original copyright line.
- **Documents describe the current state only**, in English.
