#!/bin/bash
#
# Starts TrenchBroom with the MCP server enabled.
#
# Uses the newest Release build found in any worktree of this repository
# (build*/app/TrenchBroom/trenchbroom). If there is none, configures and builds
# one in build-release/ next to this script first.
#
# Usage: start.sh [--build] [--new] [--print] [MAP_FILE ...]
#   --build   build build-release/ from the current checkout before starting
#   --new     start another editor even if one with the MCP server is running
#   --print   only print which executable would be started
#   MAP_FILE  maps to open
#
# Environment: TB_BINARY overrides the executable.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OWN_BUILD="$REPO/build-release"
DISCOVERY_FILE="$HOME/.TrenchBroom/mcp-server.json"

build=false
new=false
print=false
args=()
for arg in "$@"; do
  case "$arg" in
    --build) build=true ;;
    --new) new=true ;;
    --print) print=true ;;
    -h|--help) sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) args+=("$arg") ;;
  esac
done

notify() {
  echo "$1"
  if [ ! -t 1 ] && command -v notify-send > /dev/null; then
    notify-send -i "$REPO/app/TrenchBroom/resources/linux/icons/icon_256.png" "TrenchBroom" "$1" || true
  fi
}

build_release() {
  notify "Building TrenchBroom (Release) in $OWN_BUILD — this can take a while."
  local generator=()
  command -v ninja > /dev/null && generator=(-G Ninja)
  if [ ! -f "$OWN_BUILD/CMakeCache.txt" ]; then
    cmake -S "$REPO" -B "$OWN_BUILD" "${generator[@]}" \
      -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
  fi
  cmake --build "$OWN_BUILD" --target TrenchBroom TrenchBroomMcp --parallel
}

# Prints the newest Release editor executable of all worktrees, or nothing.
newest_release_binary() {
  local newest="" newest_time=0
  while read -r worktree; do
    for cache in "$worktree"/build*/CMakeCache.txt; do
      [ -f "$cache" ] || continue
      grep -q '^CMAKE_BUILD_TYPE:STRING=Release$' "$cache" || continue
      local binary
      binary="$(dirname "$cache")/app/TrenchBroom/trenchbroom"
      [ -x "$binary" ] || continue
      local time
      time=$(stat -c %Y "$binary")
      if [ "$time" -gt "$newest_time" ]; then
        newest="$binary"
        newest_time=$time
      fi
    done
  done < <(git -C "$REPO" worktree list --porcelain | sed -n 's/^worktree //p')
  echo "$newest"
}

if [ -n "${TB_BINARY:-}" ]; then
  binary="$TB_BINARY"
else
  if $build; then
    build_release
    binary="$OWN_BUILD/app/TrenchBroom/trenchbroom"
  else
    binary="$(newest_release_binary)"
    if [ -z "$binary" ]; then
      build_release
      binary="$OWN_BUILD/app/TrenchBroom/trenchbroom"
    fi
  fi
fi

if $print; then
  echo "$binary"
  exit 0
fi

if ! $new && [ -f "$DISCOVERY_FILE" ]; then
  pid=$(sed -n 's/.*"pid":\([0-9]*\).*/\1/p' "$DISCOVERY_FILE")
  if [ -n "$pid" ] && kill -0 "$pid" 2> /dev/null; then
    notify "TrenchBroom with the MCP server is already running (pid $pid). Use --new to start another one."
    exit 0
  fi
fi

echo "Starting $binary"
nohup "$binary" --mcp-server "${args[@]}" > /dev/null 2>&1 &
disown
