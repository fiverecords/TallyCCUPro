#!/usr/bin/env bash
# fetch_module.sh - clones the OFFICIAL Bitfocus Companion module at a fixed commit and installs
# its production dependencies. The module is run unmodified by the harness (driver/driver.js).
#
#   tests/companion/fetch_module.sh [DEST]      (default: tests/companion/module)
#
# Pinned: companion-module-fiverecords-tallyccupro v3.1.0, commit below; @companion-module/base
# 1.14.1 (the version in the module's yarn.lock). Needs git, Node 22 and npm.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DEST=${1:-${COMPANION_MODULE_DIR:-$HERE/module}}
URL=https://github.com/bitfocus/companion-module-fiverecords-tallyccupro.git
COMMIT=c35b1e77508724bfbe9de03714407dbcf5941843
BASE_VERSION=1.14.1

if [ -d "$DEST/.git" ] && [ "$(git -C "$DEST" rev-parse HEAD)" = "$COMMIT" ] && \
   [ -f "$DEST/node_modules/@companion-module/base/package.json" ]; then
  echo "module present at $COMMIT ($DEST)"
  exit 0
fi
rm -rf "$DEST"
git init -q "$DEST"
git -C "$DEST" remote add origin "$URL"
git -C "$DEST" fetch -q --depth 1 origin "$COMMIT"
git -C "$DEST" -c advice.detachedHead=false checkout -q FETCH_HEAD
version=$(node -p "require('$DEST/package.json').version")
echo "module $version at $COMMIT"
# --no-save: package.json stays untouched; the exact base version is the one of yarn.lock
(cd "$DEST" && npm install --omit=dev --ignore-scripts --no-save --no-package-lock --no-audit --no-fund \
  "@companion-module/base@$BASE_VERSION" >/dev/null)
got=$(node -p "require('$DEST/node_modules/@companion-module/base/package.json').version")
[ "$got" = "$BASE_VERSION" ] || { echo "ERROR: @companion-module/base $got, expected $BASE_VERSION" >&2; exit 1; }
if [ -n "$(git -C "$DEST" status --porcelain --untracked-files=no)" ]; then
  echo "ERROR: the module checkout was modified" >&2; exit 1
fi
echo "dependencies installed (@companion-module/base $got)"
