#!/usr/bin/env bash
# run.sh - builds and runs the host unit tests (g++).
#
#   tests/host/run.sh [--src DIR | --ref GITREF] [--out DIR] [test_*.cpp ...]
#
# The firmware is compiled with the emulator shims (tests/emulator/build.sh)
# and linked with the tests instead of the emulator's main(), so the tests
# call the real public functions. With no test files, all tests/host/test_*.cpp
# are built. The binary runs twice: with millis() from 0 and with millis()
# 1.5 s before its 32-bit wrap (EMU_MILLIS_OFFSET).
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TESTS=$(dirname "$HERE")
OUT=$TESTS/out/host
BUILD_ARGS=() FILES=()
while [ $# -gt 0 ]; do
  case "$1" in
    --src|--ref) BUILD_ARGS+=("$1" "$2"); shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
    *) FILES+=("$1"); shift ;;
  esac
done
if [ ${#FILES[@]} -eq 0 ]; then
  for f in "$HERE"/test_*.cpp; do [ "$(basename "$f")" = test_main.cpp ] || FILES+=("$f"); done
fi

"$TESTS/emulator/build.sh" "${BUILD_ARGS[@]}" --out "$OUT" --name fw
OUT=$(cd "$OUT" && pwd)
# shellcheck disable=SC1091
source "$OUT/build/fw/env.sh"

T=$OUT/build/tests; rm -rf "$T"; mkdir -p "$T"
objs=()
for o in "${EMU_OBJS[@]}"; do
  case "$(basename "$o")" in emu_runtime_*) ;; *) objs+=("$o") ;; esac
done
# Compiler output goes to $T/compile.log; shown only on errors and warnings in tests/host
log=$T/compile.log
compile() {
  if ! "$@" 2>> "$log"; then grep -E -A3 'error' "$log" | head -60; echo "host tests: BUILD FAILED (see $log)"; exit 1; fi
}
# shellcheck disable=SC2086
compile $EMU_CXX -std=gnu++17 $EMU_FLAGS $EMU_INC -Dmain=emu_runtime_main -c "$EMU_SHIM/emu_runtime.cpp" -o "$T/emu_runtime_nomain.o"
objs+=("$T/emu_runtime_nomain.o")
for f in "$HERE/test_main.cpp" "${FILES[@]}"; do
  o=$T/$(basename "$f" .cpp).o
  # shellcheck disable=SC2086
  compile $EMU_CXX -std=gnu++17 $EMU_FLAGS $EMU_INC -I"$HERE" -Wall -Wextra -Wno-unused-parameter -fpermissive -c "$f" -o "$o"
  objs+=("$o")
done
compile "$EMU_CXX" -pthread -o "$T/hosttests" "${objs[@]}"
grep -A3 "^$HERE/.*warning:" "$log" || true

run() {
  local tag=$1; shift
  local d=$T/run-$tag; mkdir -p "$d/sd"
  echo "== host tests ($tag)"
  (cd "$d" && env EMU_EEPROM="$d/eeprom.bin" EMU_SD_DIR="$d/sd" EMU_SERIAL_IN=none "$@" "$T/hosttests" > "$d/output.txt" 2>&1) && rc=0 || rc=$?
  grep -E '^\[ FAIL \]|^    FAIL |^[0-9]+ tests, ' "$d/output.txt" || true
  return $rc
}
rc=0
run millis0 || rc=1
run millis-wrap EMU_MILLIS_OFFSET=$((4294967296 - 1500)) || rc=1
[ $rc = 0 ] && echo "host tests: PASS" || echo "host tests: FAIL (full output in $T/run-*/output.txt)"
exit $rc
