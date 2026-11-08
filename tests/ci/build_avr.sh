#!/usr/bin/env bash
# build_avr.sh - compile-only AVR build of the firmware without arduino-cli, as CI does it:
# ATmega2560, core arduino:avr 1.8.6, Ethernet 2.0.2, SdFat 2.3.1 and the BMDSDIControl stub
# (tests/stubs). Reports flash, static RAM (.data + .bss + .noinit) and the sketch warnings
# (-Wall -Wextra), and fails above RAM_LIMIT bytes (default 4600).
#
#   tests/ci/build_avr.sh [--src DIR] [--out DIR]
#
# Needs avr-gcc/avr-g++ 7.3 (Debian/Ubuntu: gcc-avr avr-libc binutils-avr). Libraries come from
# tests/.cache/libs (tests/emulator/fetch_libs.sh) or ETHERNET_DIR, SDFAT_DIR, AVR_CORE_DIR.
# The image is NOT flashable: the stub does not drive the shield.
# shellcheck disable=SC2086  # flag lists are split on purpose
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TESTS=$(dirname "$HERE")
REPO=$(dirname "$TESTS")
SRC=$REPO/firmware/TallyCCUPro OUT=$TESTS/out/avr
while [ $# -gt 0 ]; do
  case "$1" in
    --src) SRC=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
RAM_LIMIT=${RAM_LIMIT:-4600}
LIBS=${TALLYCCU_LIBS:-$TESTS/.cache/libs}
if [ -z "${ETHERNET_DIR:-}" ] || [ -z "${SDFAT_DIR:-}" ] || [ -z "${AVR_CORE_DIR:-}" ]; then
  [ -d "$LIBS/SdFat/src" ] && [ -d "$LIBS/Ethernet/src" ] && [ -d "$LIBS/ArduinoCore-avr/cores" ] || "$TESTS/emulator/fetch_libs.sh" "$LIBS"
fi
ETH=${ETHERNET_DIR:-$LIBS/Ethernet}/src
SDF=${SDFAT_DIR:-$LIBS/SdFat}/src
CORE=${AVR_CORE_DIR:-$LIBS/ArduinoCore-avr}
STUB=$TESTS/stubs/BMDSDIControl/src

rm -rf "$OUT"; mkdir -p "$OUT/core" "$OUT/lib" "$OUT/sketch" "$OUT/src"
MCU="-mmcu=atmega2560 -DF_CPU=16000000L -DARDUINO=10607 -DARDUINO_AVR_MEGA2560 -DARDUINO_ARCH_AVR"
INC="-I$CORE/cores/arduino -I$CORE/variants/mega -I$CORE/libraries/SPI/src -I$CORE/libraries/Wire/src \
 -I$CORE/libraries/Wire/src/utility -I$CORE/libraries/EEPROM/src -I$ETH -I$SDF -I$STUB -I$OUT/src"
# platform.txt of arduino:avr 1.8.6
CXXF="-c -g -Os -std=gnu++11 -fpermissive -fno-exceptions -ffunction-sections -fdata-sections -fno-threadsafe-statics -Wno-error=narrowing -flto"
CF="-c -g -Os -std=gnu11 -ffunction-sections -fdata-sections -flto -fno-fat-lto-objects"

cp "$SRC"/*.h "$SRC"/*.cpp "$OUT/src/"
inos=("$SRC"/*.ino); INO=${inos[0]}
python3 "$TESTS/emulator/tools/ino2cpp.py" "$INO" "$OUT/src/$(basename "$INO").cpp" 2>/dev/null

objs=()
for f in "$CORE"/cores/arduino/*.c; do o=$OUT/core/$(basename "$f").o; avr-gcc $CF $MCU $INC -w "$f" -o "$o"; objs+=("$o"); done
for f in "$CORE"/cores/arduino/*.cpp; do o=$OUT/core/$(basename "$f").o; avr-g++ $CXXF $MCU $INC -w "$f" -o "$o"; objs+=("$o"); done
for f in "$CORE"/cores/arduino/*.S; do o=$OUT/core/$(basename "$f").o; avr-gcc -c -g -x assembler-with-cpp -flto $MCU $INC "$f" -o "$o"; objs+=("$o"); done
while IFS= read -r f; do
  o=$OUT/lib/$(printf %s "$f" | md5sum | cut -c1-8)_$(basename "$f").o
  avr-g++ $CXXF $MCU $INC -w "$f" -o "$o"; objs+=("$o")
done < <(ls "$CORE"/libraries/SPI/src/*.cpp "$CORE"/libraries/Wire/src/*.cpp; find "$ETH" "$SDF" -name '*.cpp' | sort)
for f in "$CORE"/libraries/Wire/src/utility/*.c; do o=$OUT/lib/$(basename "$f").o; avr-gcc $CF $MCU $INC -w "$f" -o "$o"; objs+=("$o"); done

: > "$OUT/warnings.txt"
fail=0
for f in "$OUT"/src/*.cpp; do
  o=$OUT/sketch/$(basename "$f").o
  avr-g++ $CXXF $MCU $INC -Wall -Wextra "$f" -o "$o" 2>> "$OUT/warnings.txt" || fail=1
  objs+=("$o")
done
[ $fail = 0 ] || { cat "$OUT/warnings.txt"; echo "SKETCH COMPILE FAILED"; exit 1; }
for o in "${objs[@]}"; do
  case $o in "$OUT"/core/*) avr-gcc-ar rcs "$OUT/core.a" "$o" ;; "$OUT"/lib/*) avr-gcc-ar rcs "$OUT/libs.a" "$o" ;; esac
done
avr-gcc -w -Os -g -flto -fuse-linker-plugin -Wl,--gc-sections $MCU -o "$OUT/TallyCCUPro.elf" "$OUT"/sketch/*.o "$OUT/libs.a" "$OUT/core.a" -lm

# static RAM as arduino-cli counts it: .data + .bss + .noinit
read -r text data bss noinit < <(avr-size -A "$OUT/TallyCCUPro.elf" | \
  awk '$1==".text"{t=$2} $1==".data"{d=$2} $1==".bss"{b=$2} $1==".noinit"{n=$2} END{print t+0, d+0, b+0, n+0}')
ram=$((data + bss + noinit))
nwarn=$(grep -c 'warning:' "$OUT/warnings.txt" || true)
nown=$(grep 'warning:' "$OUT/warnings.txt" | grep -c "^$OUT/src/" || true)
echo "flash $((text + data)) / 253952 B, static RAM $ram / 8192 B (.data $data, .bss $bss, .noinit $noinit)," \
     "warnings: $nown in the sketch, $((nwarn - nown)) from core/library headers"
[ "$nwarn" = 0 ] || grep 'warning:' "$OUT/warnings.txt" | sed "s#$OUT/src/##" | sort | uniq -c | sort -rn | head -20
avr-nm -S --size-sort -C "$OUT/TallyCCUPro.elf" | grep -iE ' [bd] ' | tail -12 > "$OUT/largest_ram_symbols.txt"
if [ "$ram" -gt "$RAM_LIMIT" ]; then
  echo "FAIL: static RAM $ram B > limit $RAM_LIMIT B"
  exit 1
fi
