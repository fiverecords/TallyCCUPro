# Tests

Everything here runs on a Linux PC without the hardware. The firmware sources are compiled
unmodified; only the layers below the libraries are replaced. CI (`.github/workflows/ci.yml`)
runs the same commands.

| Directory | What | Time |
|---|---|---|
| `stubs/BMDSDIControl/` | compile-only stub of the proprietary Blackmagic library (Arduino library layout) | - |
| `ci/` | AVR build and RAM check, sketch report parser, web page checks, firmware version check | 10 s |
| `host/` | unit tests (g++) of pure functions through their public API | 10 s |
| `emulator/` | the firmware as a Linux program: emulated W5500, SD, EEPROM, watchdog, BMD shield | 5 s build |
| `scenarios/` | robustness scenarios against the emulator (network faults, slow clients, background transfers, SD, Safe Mode, reset causes, CSRF) | 6 min fast set, 20 min all |
| `companion/` | the official Companion module v3.1.0 driving the emulator; differential runs between versions | 3 min subset, 10 min full |

Requirements: bash, git, python3, g++ (9+), Node 22 and npm (Companion), avr-gcc 7.3 for the
AVR build (`apt install gcc-avr avr-libc binutils-avr`). Generated files go to `tests/out/` and
downloads to `tests/.cache/` and `tests/companion/module/` (all git-ignored).

## Pinned dependencies

| Dependency | Version | Where |
|---|---|---|
| arduino:avr core | 1.8.6 (ArduinoCore-avr `42fa4a1`) | `emulator/fetch_libs.sh`, CI |
| Ethernet | 2.0.2 (`d5672b7`) | `emulator/fetch_libs.sh`, CI |
| SdFat | 2.3.1 (`cda0573`) | `emulator/fetch_libs.sh`, CI |
| Companion module | v3.1.0, commit `c35b1e7` | `companion/fetch_module.sh` |
| @companion-module/base | 1.14.1 | `companion/fetch_module.sh` |
| arduino-cli | 1.3.1 | CI (`arduino/compile-sketches`) |

## Firmware build and RAM

```bash
tests/emulator/fetch_libs.sh      # once: pinned libraries -> tests/.cache/libs
tests/ci/build_avr.sh             # ATmega2560 build with the stub: flash, static RAM, warnings
RAM_LIMIT=4600 tests/ci/build_avr.sh --src path/to/sketch
```

Static RAM is `.data + .bss + .noinit` (what arduino-cli calls global variables) measured with the stub (the real BMD library adds its own), and the
build fails above `RAM_LIMIT` (4600 B). CI also builds with arduino-cli through
`arduino/compile-sketches` (same core and library versions) and reads its report with
`ci/sketch_report.py`. The stub does not drive the shield: never flash those builds.

For the stack depth of a function, compile its file without LTO and read the `.su` file:
`avr-g++ -c -Os -mmcu=atmega2560 -fstack-usage ... file.cpp`.

## Host unit tests

```bash
tests/host/run.sh                         # all tests/host/test_*.cpp
tests/host/run.sh tests/host/test_netutils.cpp
tests/host/run.sh --ref HEAD~1            # against the firmware of another commit
```

The firmware is compiled with the emulator shims and linked with the tests instead of the
emulator's `main()`. Covered: `CCUControl::parseFloatList`, `parseBool`, `validateValue` and the
parameter table lookups; `NetUtils::LineReader`, `readLine`, `drainInput`, the deadlines (also
across the 32-bit `millis()` wrap: the binary runs twice, the second time 1.5 s before the wrap)
and `ClientBufferPrint`, with a scripted `EthernetClient` (`mock_client.h`);
`WebServer::printJsonSafe`; the `TallyManager` input map; `NetworkManager::parseIP`,
`isHostIP` and `isSubnetMask`; the factory settings of a blank EEPROM (`StorageManager`: the
defaults documented in the main README, the 0/255 first-octet rule, settings of older versions
kept, `factoryReset` keeping the MAC). Add a `TEST(name) { CHECK(...); }`
in a new `test_*.cpp` (see `test.h`). Private functions (the vMix tally parser, the URL
decoder) are covered end to end by the scenarios and the Companion runs instead.

## Emulator and scenarios

```bash
tests/emulator/build.sh                   # -> tests/out/emu/bin/emu (see emulator/README.md)
sudo sysctl -w net.ipv4.ip_unprivileged_port_start=0    # or run as root
python3 tests/scenarios/run.py --list
python3 tests/scenarios/run.py --fast     # csrf misc k fh i d g boot coop coop2
sudo -E python3 tests/scenarios/run.py --all            # + slow clients, network cuts, Safe Mode
python3 tests/scenarios/run.py misc csrf
```

Each scenario starts its own emulator, fake vMix and clients, prints a JSON result and is
checked by `run.py` (results and logs in `tests/out/scenarios/`). Any emulated watchdog reset
fails a scenario, except in `j` (Safe Mode) and `boot` (reset causes), which reset on purpose. The scenarios marked
`root` cut connections with iptables. Addresses: `T_IP` (firmware, default 127.0.80.1),
`T_VMIX` (fake vMix, 127.0.80.11) and `T_PEER_PREFIX` (other clients, 127.0.81.x), so several
runs can share a machine; `EMU_BIN` selects the binary.

| Scenario | Checks |
|---|---|
| `csrf` | SEC-01/SEC-06: state-changing endpoints need `X-TallyCCU: 1` (403 otherwise, nothing applied), the Companion and read endpoints do not, no CORS header, OPTIONS refused |
| `misc` | one response per request, stale `.gz` after upload, tally slot clear, tally off with vMix disabled |
| `k` | an interrupted `savePreset` keeps the previous preset |
| `fh` | TALLY lines split across segments; PTZ deadman with and without `deadman=1` |
| `i` | SD absent at boot and pulled at runtime |
| `d` | 8 idle connections on ports 80 and 8098 are reclaimed; web, Companion and vMix recover |
| `g` | two SSE browsers, no eviction loop, `REQUESTSYNC` at most every 10 s |
| `b`, `bgz`, `b2` | slow page downloads (plain and gzip) do not stall tally or the PTZ stop |
| `a`, `c` (root) | an SSE client or Companion vanishes mid-transfer |
| `e`, `e2` (root) | vMix half-open or unreachable: tally off, reconnect, HTTP and 8098 stay responsive |
| `j` | Safe Mode: shield absent, automatic recovery, anti-loop strikes |
| `boot` | reset causes (power-up, restart, reset pin, watchdog in operation or during boot): only hangs in the 20 s boot window count, Safe Mode after 3, a power cycle boots normally |
| `coop`, `coopgz` | background transfers: Companion-style PTZ stop and API under 100/150 ms during a 2 KB/s page download, byte-exact parallel downloads, a third page waits for a slot (or `503` after 10 s with the sockets short), stalled and slow readers cut with FIN, delete during a transfer, SSE tally events |
| `coop2` | transfers during a slow upload: not cut, byte exact; replacing the file being sent cuts that transfer; SSE events keep flowing |
| `upl` | tally latency around the end of slow uploads |

## Companion module

See `companion/README.md`: fetch the module, generate the plans, run it against the emulator,
and compare two firmware versions case by case.

## Web pages

```bash
python3 tests/ci/check_pages.py           # node --check of every inline <script>, line endings per page
python3 tests/ci/check_version.py         # the firmware version is only in FIRMWARE_VERSION (DOC-12)
```

`sdcard/index.html` uses CRLF line endings and the other pages LF: keep them (the check fails otherwise; a new CRLF page goes in `EXPECTED_EOL`).
