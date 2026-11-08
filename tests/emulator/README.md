# Host emulator (Arduino Mega 2560 + W5500 + SD + BMD shield)

Builds the **unmodified** firmware sources as a Linux program, so network, timing and
watchdog behaviour can be tested without hardware. Compiled as is: the sketch, the real
Ethernet 2.0.2 library (including `utility/w5100.cpp`) and the AVR core's Print, Stream,
WString, IPAddress and WMath (arduino:avr 1.8.6). Everything below the libraries is
emulated by the shims in `shim/`.

```bash
tests/emulator/fetch_libs.sh                  # pinned Ethernet, SdFat, ArduinoCore-avr -> tests/.cache/libs
tests/emulator/build.sh                       # firmware/TallyCCUPro -> tests/out/emu/bin/emu
tests/emulator/build.sh --ref 8d554d5 --name emu-371   # v3.7.1 (Arduino/ tree) from git
tests/emulator/build.sh --src DIR --out DIR --name NAME
```

`build.sh` fetches the libraries itself when they are missing. `ETHERNET_DIR` and
`AVR_CORE_DIR` (library roots) use existing copies instead; `CXX` picks the compiler (g++ 9 or
newer). Compiler output goes to `OUT/build/NAME/build.log`. Linux only (it uses
`TCP_USER_TIMEOUT`, `prctl` and FIFOs).

## Running it

The binary takes no arguments; everything is set through the environment. Minimal run:

```bash
mkdir -p /tmp/sd && cp sdcard/*.html /tmp/sd/
EMU_BIND_IP=127.0.80.1 EMU_SD_DIR=/tmp/sd EMU_EEPROM=/tmp/eeprom.bin EMU_SERIAL_IN=none \
  tests/out/emu/bin/emu
# http://127.0.80.1/ and TCP 127.0.80.1:8098
```

Port 80 needs root or `sudo sysctl -w net.ipv4.ip_unprivileged_port_start=0`.
Serial TX goes to stdout; `[EMU] ...` lines (boots, watchdog) go to stderr.

| Variable | Meaning |
|---|---|
| `EMU_BIND_IP` | address of the emulated board (any 127.x.y.z); default 127.0.0.1 |
| `EMU_SD_DIR` | directory used as the SD card; unset: `SD.begin()` fails |
| `EMU_EEPROM` | 4 KB EEPROM file, created as 0xFF (default `emu-eeprom.bin`) |
| `EMU_SERIAL_IN` | serial RX: a FIFO, a file or `none` (default stdin) |
| `EMU_REMAP_ALL` / `EMU_REMAP` | redirect outgoing connections (vMix): all to one IP, or `a.b.c.d=w.x.y.z,...` |
| `EMU_BMD_LOG` `EMU_NET_LOG` `EMU_SD_LOG` | JSON-lines traces: every BMD call (what reaches the camera), network events, SD operations |
| `EMU_SHIELD_FILE` | file whose first byte `0` makes the SDI shield absent (`begin()` hangs, I2C NACK) |
| `EMU_HANG_FILE` | first byte `1`: every tally write hangs (shield stuck in operation) until it changes |
| `EMU_TALLY_FAIL_FILE` | first byte `1`: tally writes are not acknowledged (`setCameraTally()` returns false) |
| `EMU_SHIELD_RESET_FILE` | first byte `1`: the shield lost its settings (override reads off until set again) |
| `EMU_NOINIT_POWERON=zero` | zeros instead of noise in `.noinit` RAM at power-up |
| `EMU_SD_FAIL`, `EMU_SD_READONLY`, `EMU_SD_FAIL_BEGIN_MS` | SD faults |
| `EMU_MAX_BOOTS`, `EMU_EXIT_AFTER_MS` | stop after N boots / after a time (safety net for tests) |
| `EMU_WDT_EXIT=1` | exit with code 42 on a watchdog reset instead of rebooting |
| `EMU_WDT_DISABLE=1` | no watchdog |
| `EMU_MILLIS_OFFSET` | start `millis()` at this value (e.g. near the 32-bit wrap) |
| `EMU_MCUSR_KEEP=1` | `MCUSR` holds the reset cause (PORF, EXTRF, WDRF); default 0, as the Mega bootloader clears it |
| `EMU_EXTRA_FLAGS` (build) | extra compiler flags for `build.sh`, e.g. `-DDEBUG_HOT=1` |
| `EMU_FREE_RAM` | what `freeRam()` reports (default 2000; the real RAM is not modelled) |
| `EMU_RCVBUF` / `EMU_SNDBUF` | host socket buffers (default 4 KB) |
| `EMU_REALTIME=0` | no pacing of SPI, EEPROM and serial (faster, less faithful) |
| `EMU_VERBOSE=1` | more `[EMU]` logging |

Every log line carries `wt` (epoch ms), so it can be matched with the time windows recorded by
the test harnesses.

Resets: `SIGUSR1` is the reset pin (button, serial DTR); a watchdog timeout or the firmware's
own restart resets by watchdog; stopping and starting the process is a power cycle. A reset
re-executes the binary and hands it the `.noinit` RAM section (the firmware's boot record), as
the AVR keeps RAM across a reset; a power-up fills it with noise.

## What is emulated

| Piece | File | Notes |
|---|---|---|
| Time, pins, watchdog, reset | `shim/emu_runtime.cpp` | `millis()`/`micros()` wrap at 32 bits. The watchdog is simulated: with `WDIE` set the first timeout runs the firmware's `WDT_vect` (`[EMU] WDT interrupt after N ms`) and the next one resets; a reset prints `[EMU] WATCHDOG RESET after N ms` and re-executes the binary with the boot count + 1. |
| `.noinit` RAM | `shim/emu_noinit.cpp` | found in the ELF section table, passed to the next boot in `EMU_NOINIT`, pseudo-random after a power-up. |
| Serial | `shim/emu_serial.cpp` | TX to stdout, paced at the baud rate; RX from `EMU_SERIAL_IN`. |
| EEPROM | `shim/emu_eeprom.cpp` | file backed, 3.4 ms per written byte. |
| W5500 | `shim/emu_w5500.cpp` | emulated at the **SPI register** level; the Ethernet library talks to it unchanged. Each chip socket maps to a host TCP socket bound to `EMU_BIND_IP`. W5500 register map (RTR 0x19, RCR 0x1B). The retransmission bound becomes `TCP_USER_TIMEOUT`. LISTEN without a free socket answers RST. CLOSE leaves the peer half-open. A CONNECT to an address the host cannot route is never answered (ARP timeout). |
| SD (SdFat 2.x API) | `shim/libs/SdFat` | backed by `EMU_SD_DIR`, with SdFat semantics (FILE_WRITE appends, rename fails if the target exists, write errors on closed files). |
| BMD 3G-SDI shield | `shim/libs/BMDSDIControl` | same API as the compile stub (`tests/stubs`); every call is logged. Fixed16 values are logged as float and as the 5.11 integer. |
| Wire | `shim/emu_wire.cpp` | I2C NACK when the shield is absent. |
| libc | `shim/Arduino.h` | `atoi`/`atol`/`strtol`/`strtoul` behave as on the Mega (16/32 bit); `strcpy`/`memcpy`/... are forward byte loops as in avr-libc. |

## Limits

- `int` is 32 bits, `long` 64 and `double` 64 (only the number parsers are AVR-like). Static RAM
  and stack are not modelled: measure them with `tests/ci/build_avr.sh` and `-fstack-usage`.
- The watchdog interrupt runs in the watchdog thread while the sketch keeps running, and the host
  `WDT_vect` returns (the AVR one never does), so no hang address is recorded.
- The BMD library is proprietary: only the call made by the firmware is visible, not the I2C bytes.
- W5500: SEND_OK is set when the host kernel accepts the data; the host socket buffers add to the
  chip's 2 KB; retransmissions are done by Linux. An emulated reset closes the host sockets (a real
  W5500 keeps them half-open until its soft reset). UDP, DHCP and DNS are not emulated.
- SD timing, write cache and FAT directory order are not modelled; listings are sorted by name.
