#!/usr/bin/env bash
# fetch_libs.sh - fetches the pinned third-party sources the test tooling needs.
#
#   tests/emulator/fetch_libs.sh [DEST]
#
# DEST defaults to $TALLYCCU_LIBS or tests/.cache/libs. Each library is a
# shallow clone of a FIXED tag, checked against the expected commit:
#   Ethernet         2.0.2  (arduino-libraries/Ethernet)
#   SdFat            2.3.1  (greiman/SdFat)
#   ArduinoCore-avr  1.8.6  (arduino/ArduinoCore-avr; same as arduino:avr@1.8.6)
# These are the versions the firmware is built and tested with (CI pins the
# same ones). Already present and at the right commit: nothing is fetched.
#
# Instead of fetching, the build scripts accept existing copies through
# ETHERNET_DIR, SDFAT_DIR and AVR_CORE_DIR (library roots, not src/).
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TESTS=$(dirname "$HERE")
DEST=${1:-${TALLYCCU_LIBS:-$TESTS/.cache/libs}}
mkdir -p "$DEST"

# name  url  tag  commit
LIBS=(
  "Ethernet https://github.com/arduino-libraries/Ethernet.git 2.0.2 d5672b707a85874bf69a312b8dce11df710a14d3"
  "SdFat https://github.com/greiman/SdFat.git 2.3.1 cda057318bec196183d4cc92b01bc1dd64bbfb02"
  "ArduinoCore-avr https://github.com/arduino/ArduinoCore-avr.git 1.8.6 42fa4a1ea1b1b11d1cc0a60298e529d37f9d14bd"
)

for entry in "${LIBS[@]}"; do
  read -r name url tag commit <<<"$entry"
  dir=$DEST/$name
  if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ]; then
    echo "$name $tag: present ($dir)"
    continue
  fi
  rm -rf "$dir"
  echo "$name $tag: cloning $url"
  git -c advice.detachedHead=false clone -q --depth 1 --branch "$tag" "$url" "$dir"
  got=$(git -C "$dir" rev-parse HEAD)
  if [ "$got" != "$commit" ]; then
    echo "ERROR: $name tag $tag is $got, expected $commit (tag moved?)" >&2
    exit 1
  fi
done
echo "libraries in $DEST"
