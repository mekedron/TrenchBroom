# TrenchBroom with MCP

[![TrenchBroom Icon](app/TrenchBroom/resources/graphics/images/AppIcon.png)](https://www.youtube.com/watch?v=shcAvnYp9ow)

> [!IMPORTANT]
> **This is a fork with an MCP server for AI agents.** An agent like Claude Code can open this editor and build maps with you: geometry, entities and NPCs, textures that look right, snapshots from its own cameras to check its work, compiling, and starting the game. Highlights:
>
> - **198 tools, 9 prompts**, 98.7% of the editor's actions reachable
> - **Agent vision**: offscreen snapshots from the agent's own cameras; pick objects by pixel
> - **Understands space**: rooms, openings, free spots on walls and floors, walkability
> - **Catches problems as it goes**: z-fighting, leaks before compiling, models stuck in furniture, stretched textures
> - **Texturing knowledge**: learns how a game's original maps use each texture
> - **Compiles and plays**: Half-Life, Quake, Quake 2 and Quake 3 presets
> - **Every edit is one Undo step**, live in your editor window
>
> Fun fact: **not a single line of the MCP code has been reviewed by a human.** It was written by AI agents as an experiment. With all respect to TrenchBroom's author, who rightly asks contributors to understand the code they submit, this lives in a fork rather than a pull request. It is not a serious contribution, but it is quite usable, and it turned out really cool.
>
> **[Read the MCP server overview →](docs/mcp/OVERVIEW.md)**

TrenchBroom is a modern cross-platform level editor for Quake-engine based games.

- Trailer:   https://www.youtube.com/watch?v=shcAvnYp9ow
- Website:   https://github.com/TrenchBroom/TrenchBroom
- Discord:   https://discord.gg/WGf9uve
- Mastodon:  https://mastodon.gamedev.place/@trenchbroom
- Bluesky:   https://bsky.app/profile/trenchbroom.bsky.social
- Video Tutorial Series:  https://www.youtube.com/playlist?list=PLgDKRPte5Y0AZ_K_PZbWbgBAEt5xf74aE
- Manual:    https://trenchbroom.github.io/manual/latest

## Features
* **General**
  - Full support for editing in 3D and in up to three 2D views
  - High performance renderer with support for huge maps
  - Unlimited Undo and Redo
  - Macro-like command repetition
  - Issue browser with automatic quick fixes
  - Point file support
  - Automatic backups
  - .obj file export
  - Free and cross platform
* **Brush Editing**
  - Robust vertex editing with edge and face splitting and manipulating multiple vertices together
  - Clipping tool with two and three points
  - Scaling and shearing tools
  - CSG operations: merge, subtract, intersect
  - UV view for easy texture manipulations
  - Precise texture lock for all brush editing operations
  - Multiple material collections
* **Entity Editing**
  - Entity browser with drag and drop support
  - Support for FGD and DEF files for entity definitions
  - Mod support
  - Entity link visualization
  - Displays 3D models in the editor
  - Smart entity property editors
* **Supported Games**
  - Quake (Standard and Valve 220 file formats)
  - Quake 2
  - Quake 3 (partial, no patches or brush primitives yet)
  - Hexen 2
  - Daikatana
  - Generic (for custom engines)
  - More games can be supported with custom game configurations


## Releases

Binary builds are available from [releases](https://github.com/kduske/TrenchBroom/releases).

## Compiling

Read [BUILD.md](BUILD.md) for instructions.

# Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for more information.

# Credits
- [Qt](https://www.qt.io/)
- [FreeType](https://www.freetype.org/)
- [FreeImage](https://freeimage.sourceforge.io/)
- [TinyXML](http://www.grinninglizard.com/tinyxml/)
- miniz
- [Assimp](https://www.assimp.org/)
- [Catch2](https://github.com/catchorg/Catch2)
- [CMake](https://cmake.org/)
- [Pandoc](https://www.pandoc.org/)
- Quake icons by [Th3 ProphetMan](https://www.deviantart.com/th3-prophetman)
- Hexen 2 icon by [thedoctor45](https://www.deviantart.com/thedoctor45)
- [Source Sans Pro](https://fonts.google.com/specimen/Source+Sans+Pro) font

## Changes

See [releases](https://github.com/TrenchBroom/TrenchBroom/releases) for latest changes.
