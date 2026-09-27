#!/usr/bin/env bash
#
# Copyright (C) 2026 Nikita Rabykin
#
# This file is part of TrenchBroom.
#
# TrenchBroom is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# TrenchBroom is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
#
# Reports the upstream footprint of this fork:
#
#  1. Lists every original TrenchBroom file that the fork changes (files that exist in the
#     merge base with upstream master and differ from it), with added and removed line
#     counts, and counts the files that the fork adds.
#  2. Checks that a merge with the latest upstream master has no conflicts. The merge is
#     computed with `git merge-tree`, which only writes objects to the object database: it
#     changes neither the working tree, nor the index, nor any branch.
#
# Usage: scripts/upstream-footprint.sh [--worktree] [--no-fetch]
#
#   --worktree  Report the current working tree (including uncommitted and untracked,
#               non-ignored files) instead of HEAD. The working tree is snapshotted into a
#               temporary commit object through a temporary index; the real index and all
#               refs stay untouched.
#   --no-fetch  Do not run `git fetch` for the upstream remote first.
#
# Environment: UPSTREAM_REMOTE (default: origin), UPSTREAM_BRANCH (default: master).
#
# Exit status: 0 if the merge is clean, 1 if it has conflicts, 2 on usage or git errors.

set -euo pipefail

remote="${UPSTREAM_REMOTE:-origin}"
branch="${UPSTREAM_BRANCH:-master}"
use_worktree=false
fetch=true

for arg in "$@"; do
  case "$arg" in
    --worktree) use_worktree=true ;;
    --no-fetch) fetch=false ;;
    -h|--help)
      sed -n '20,39p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "Unknown option: $arg" >&2
      exit 2
      ;;
  esac
done

cd "$(git rev-parse --show-toplevel)"

upstream="refs/remotes/$remote/$branch"
if $fetch; then
  echo "Fetching $remote/$branch..."
  git fetch --quiet "$remote" "+refs/heads/$branch:$upstream"
fi
git rev-parse --verify --quiet "$upstream^{commit}" >/dev/null || {
  echo "Upstream branch $upstream not found; run without --no-fetch." >&2
  exit 2
}

# The commit whose footprint is reported: HEAD, or a snapshot of the working tree.
ours="$(git rev-parse HEAD)"
ours_label="HEAD"
if $use_worktree; then
  tmp_index="$(mktemp)"
  trap 'rm -f "$tmp_index"' EXIT
  # Build the snapshot in a temporary index so that the user's index is not modified.
  GIT_INDEX_FILE="$tmp_index" git read-tree HEAD
  GIT_INDEX_FILE="$tmp_index" git add --all
  tree="$(GIT_INDEX_FILE="$tmp_index" git write-tree)"
  ours="$(git commit-tree "$tree" -p HEAD -m "upstream-footprint working tree snapshot")"
  ours_label="working tree"
fi

base="$(git merge-base "$ours" "$upstream")"

echo
echo "Merge base with $remote/$branch: $(git log -1 --format='%h %s' "$base")"
echo "Latest $remote/$branch:        $(git log -1 --format='%h %s' "$upstream")"
echo "Reported state:                $ours_label"

# Changed upstream files: files that exist in the merge base and are modified or deleted.
# Renames are disabled so that a moved upstream file shows up as a deletion.
echo
echo "Changed upstream files (added/removed lines):"
changed_files=0
added_lines=0
removed_lines=0
while IFS=$'\t' read -r added removed path; do
  printf '  %+6s %6s  %s\n' "+$added" "-$removed" "$path"
  changed_files=$((changed_files + 1))
  if [[ "$added" != "-" ]]; then
    added_lines=$((added_lines + added))
    removed_lines=$((removed_lines + removed))
  fi
done < <(git diff --numstat --no-renames --diff-filter=MDT "$base" "$ours")

if [[ $changed_files -eq 0 ]]; then
  echo "  (none)"
fi
new_files="$(git diff --name-only --no-renames --diff-filter=A "$base" "$ours" | wc -l)"
echo "  $changed_files changed upstream files, +$added_lines/-$removed_lines lines;" \
  "$new_files files added by the fork"

# Test merge with the latest upstream master.
echo
echo "Test merge of $ours_label with $remote/$branch:"
set +e
merge_output="$(git merge-tree --write-tree --name-only --messages "$ours" "$upstream")"
merge_status=$?
set -e

case $merge_status in
  0)
    echo "  clean, no conflicts"
    exit 0
    ;;
  1)
    echo "  CONFLICTS:"
    # The first line is the tree id; conflicted paths follow until the first empty line,
    # then the informational messages.
    printf '%s\n' "$merge_output" | tail -n +2 | sed 's/^/    /'
    exit 1
    ;;
  *)
    echo "  git merge-tree failed:" >&2
    printf '%s\n' "$merge_output" >&2
    exit 2
    ;;
esac
