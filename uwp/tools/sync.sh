#!/bin/bash
# sync.sh -- mirror the canonical source (this repository, a Windows checkout edited from Windows) into the WSL
# build tree (~/xemu-uwp/xemu, a clone of upstream xemu at XEMU_BASE, where building is fast).
#
# The canonical checkout is Windows git's, with core.autocrlf=true: its files have CRLF line ends, so WSL's git
# would call EVERY file modified (it once copied all 11k files over the build tree -- CRLF shell scripts do not
# run). So the list of files comes from Windows git (git.exe): everything that differs from the upstream release
# the port is based on (committed or not) plus new files. Each one is copied with CR stripped, only when it
# differs, so ninja rebuilds just what changed.
set -eu
SRC=$(cd "$(dirname "$0")/../.." && pwd)
DST=~/xemu-uwp/xemu
BASE="${XEMU_BASE:-v0.8.137}"
cd "$SRC"
{ git.exe diff --name-only "$BASE"; git.exe ls-files --others --exclude-standard; } | tr -d '\r' | sort -u |
while read -r f; do
  [ -f "$f" ] || continue
  tmp=$(mktemp)
  sed 's/\r$//' "$f" > "$tmp"
  if ! cmp -s "$tmp" "$DST/$f"; then
    mkdir -p "$(dirname "$DST/$f")"
    cat "$tmp" > "$DST/$f"
    echo "  $f"
  fi
  rm -f "$tmp"
done
