#!/usr/bin/env bash
# build.sh - builds the TallyCCU Pro firmware as a Linux program (emulator).
#
#   tests/emulator/build.sh [--src DIR | --ref GITREF] [--out DIR] [--name NAME]
#
#   --src DIR     firmware sources (default: firmware/TallyCCUPro of this repo)
#   --ref GITREF  firmware of a git commit/tag/branch of this repo instead
#                 (firmware/TallyCCUPro, or Arduino/ for v3.7.1 and older)
#   --out DIR     output root (default: tests/out/emu)
#   --name NAME   binary name (default: emu) -> OUT/bin/NAME
#
# The sources are compiled unmodified, with the real Ethernet 2.x library and
# the AVR core's Print/Stream/WString/IPAddress. Libraries: ETHERNET_DIR and
# AVR_CORE_DIR (roots), else tests/.cache/libs (fetched by fetch_libs.sh,
# which runs automatically when they are missing). CXX defaults to g++;
# EMU_EXTRA_FLAGS adds compiler flags (e.g. -DDEBUG_HOT=1).
#
# Also writes OUT/build/NAME/env.sh (compiler, flags, objects) for the host
# unit tests (tests/host/run.sh).
# shellcheck disable=SC2086  # flag lists are split on purpose
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TESTS=$(dirname "$HERE")
REPO=$(dirname "$TESTS")

SRC="" REF="" OUTD="$TESTS/out/emu" NAME=emu
while [ $# -gt 0 ]; do
  case "$1" in
    --src) SRC=$2; shift 2 ;;
    --ref) REF=$2; shift 2 ;;
    --out) OUTD=$2; shift 2 ;;
    --name) NAME=$2; shift 2 ;;
    -h|--help) sed -n '2,21p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[ -n "$SRC" ] && [ -n "$REF" ] && { echo "--src and --ref are exclusive" >&2; exit 2; }

LIBS=${TALLYCCU_LIBS:-$TESTS/.cache/libs}
if [ -z "${ETHERNET_DIR:-}" ] || [ -z "${AVR_CORE_DIR:-}" ]; then
  if [ ! -d "$LIBS/Ethernet/src" ] || [ ! -d "$LIBS/ArduinoCore-avr/cores" ]; then
    "$HERE/fetch_libs.sh" "$LIBS"
  fi
fi
ETHERNET_DIR=${ETHERNET_DIR:-$LIBS/Ethernet}
AVR_CORE_DIR=${AVR_CORE_DIR:-$LIBS/ArduinoCore-avr}
ETH=$ETHERNET_DIR/src
CORE=$AVR_CORE_DIR/cores/arduino
EEP=$AVR_CORE_DIR/libraries/EEPROM/src
for d in "$ETH" "$CORE" "$EEP"; do [ -d "$d" ] || { echo "missing $d" >&2; exit 1; }; done

OUTD=$(mkdir -p "$OUTD" && cd "$OUTD" && pwd)
B=$OUTD/build/$NAME
rm -rf "$B"; mkdir -p "$B/fw" "$B/core" "$B/obj" "$OUTD/bin"

if [ -n "$REF" ]; then
  if git -C "$REPO" cat-file -e "$REF:firmware/TallyCCUPro" 2>/dev/null; then FWDIR=firmware/TallyCCUPro; else FWDIR=Arduino; fi
  depth=$(($(printf %s "$FWDIR" | tr -cd / | wc -c) + 1))
  git -C "$REPO" archive "$REF" "$FWDIR" | tar -x -C "$B/fw" --strip-components="$depth"
  echo "$(git -C "$REPO" rev-parse "$REF") $FWDIR" > "$B/REV"
else
  SRC=${SRC:-$REPO/firmware/TallyCCUPro}
  cp -r "$SRC"/. "$B/fw/"
  { git -C "$REPO" rev-parse HEAD 2>/dev/null || true; echo "$SRC"; } > "$B/REV"
fi
inos=("$B"/fw/*.ino); INO=${inos[0]}
[ -f "$INO" ] || { echo "no .ino in the firmware sources" >&2; exit 1; }
# Old trees include "WebServer.h" while the file is webserver.h
if [ -f "$B/fw/webserver.h" ] && [ ! -e "$B/fw/WebServer.h" ]; then ln -s webserver.h "$B/fw/WebServer.h"; fi

for f in Print.cpp Print.h Printable.h Stream.cpp Stream.h WString.cpp WString.h IPAddress.cpp IPAddress.h \
         WCharacter.h Client.h Server.h Udp.h binary.h WMath.cpp; do cp "$CORE/$f" "$B/core/"; done
# Host unsigned long is 64-bit: keep IPAddress(0ul) unambiguous as on AVR
sed -i 's#^\(\s*\)IPAddress(uint32_t address);#&\n\1IPAddress(unsigned long address) : IPAddress((uint32_t)address) {}#' "$B/core/IPAddress.h"
python3 "$HERE/tools/ino2cpp.py" "$INO" "$B/fw/$(basename "$INO").cpp" 2>/dev/null

CXX=${CXX:-g++}
FLAGS="-g -O1 -fno-omit-frame-pointer -pthread -DARDUINO=10819 -DARDUINO_AVR_MEGA2560 -DARDUINO_ARCH_AVR -DF_CPU=16000000L -DEMU_BUILD=1 \
 -Wno-unused-parameter -Wno-deprecated-declarations -Wno-write-strings -Wno-narrowing ${EMU_EXTRA_FLAGS:-}"
INC="-I$HERE/shim -I$B/core -I$HERE/shim/libs/SdFat -I$HERE/shim/libs/BMDSDIControl -I$ETH -I$EEP -I$B/fw"
FWFLAGS="-Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -Wno-int-to-pointer-cast -fpermissive"

JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}
OBJS=() FAILS="$B/failed"; : > "$FAILS"
cc() {
  local src=$1; shift
  local o; o=$B/obj/$(basename "$src" .cpp)_$(printf %s "$src" | md5sum | cut -c1-6).o
  OBJS+=("$o")
  ( $CXX -std=gnu++17 $FLAGS $INC "$@" -c "$src" -o "$o" 2> "$o.log" || echo "$o.log" >> "$FAILS" ) &
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n || true; done
}
for f in "$HERE"/shim/*.cpp "$HERE"/shim/libs/SdFat/*.cpp "$HERE"/shim/libs/BMDSDIControl/*.cpp; do cc "$f"; done
for f in "$B"/core/*.cpp; do cc "$f" -w -include Arduino.h; done
for f in "$ETH"/*.cpp "$ETH"/utility/w5100.cpp; do cc "$f" -w; done
for f in "$B"/fw/*.cpp; do cc "$f" $FWFLAGS; done
wait
cat "$B"/obj/*.log > "$B/build.log"
if [ -s "$FAILS" ]; then
  echo "COMPILE FAILED:" >&2
  while read -r l; do grep -E -A3 "error" "$l" | head -40 >&2; done < "$FAILS"
  exit 1
fi

$CXX -pthread -o "$OUTD/bin/$NAME" "${OBJS[@]}"
{
  printf 'EMU_CXX=%q\nEMU_FLAGS=%q\nEMU_INC=%q\nEMU_FWFLAGS=%q\nEMU_SHIM=%q\nEMU_FW=%q\n' "$CXX" "$FLAGS" "$INC" "$FWFLAGS" "$HERE/shim" "$B/fw"
  printf 'EMU_OBJS=('; printf '%q ' "${OBJS[@]}"; printf ')\n'
} > "$B/env.sh"
echo "built $OUTD/bin/$NAME from $(head -1 "$B/REV") ($(grep -c 'warning:' "$B/build.log" || true) compiler warnings, see $B/build.log)"
