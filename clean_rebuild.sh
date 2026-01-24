#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Clean top-level qmake artifacts
rm -f "$ROOT/Makefile" "$ROOT/.qmake.cache" "$ROOT/.qmake.stash" "$ROOT/config.log"
rm -rf "$ROOT/config.tests"

# Clean in-tree subproject build outputs
for dir in moonlight-common-c qmdnsengine h264bitstream app; do
  rm -f "$ROOT/$dir/Makefile" "$ROOT/$dir/Makefile.Debug" "$ROOT/$dir/Makefile.Release" \
        "$ROOT/$dir/.qmake.cache" "$ROOT/$dir/.qmake.stash"
  rm -rf "$ROOT/$dir/release" "$ROOT/$dir/debug"
done

rm -f "$ROOT/app/moonlight"

# Rebuild
qmake6 "$ROOT/moonlight-qt.pro"
make -C "$ROOT" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
