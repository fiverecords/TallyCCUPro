# TallyCCU Pro


![Front](Images/Color%20Wheels.png)

<p align="center">
  <strong>Open-source CCU control system for Blackmagic Design cameras with vMix tally integration using the Blackmagic Arduino SDI Shield</strong>
</p>

TallyCCU Pro is an Arduino-based solution that provides full Camera Control Unit (CCU) functionality for Blackmagic cameras via SDI using the Blackmagic Arduino SDI Shield, along with real-time tally light integration with vMix. Control your cameras from a web browser, Bitfocus Companion, or build your own integration using the TCP API.

---

## Features

### Camera Control
- **Full CCU Parameters**: Iris, focus, zoom, white balance, ISO, shutter, ND filters
- **Color Correction**: Lift, gamma, gain, offset with visual color wheels
- **Display Settings**: Zebra, peaking, focus assist, LUT selection
- **Audio Control**: Mic levels, headphone mix, phantom power
- **PTZ Support**: Pan/tilt velocity and memory presets (for supported cameras)

### Tally System
- **vMix Integration**: Automatic tally lights from vMix program/preview state
- **Flexible Mapping**: Map any vMix input to any camera ID
- **Adjustable Brightness**: Independent front/rear tally brightness control

### Connectivity
- **Web Interface**: Full control from any browser on your network
- **Companion Module**: Stream Deck integration via Bitfocus Companion
- **TCP API**: Real-time bidirectional sync on port 8098
- **Serial Configuration**: Initial setup without network access

### Real-Time Synchronization (v3.5+)
- **Server-Sent Events (SSE)**: Instant parameter sync between web interface and Companion
- **Bidirectional Sync**: Changes in Companion appear instantly in the web interface and vice versa
- **Per-Camera State**: Browser maintains state for all 8 cameras, enabling seamless switching
- **Automatic State Recovery**: When either client connects, it receives current state from the other
- **Live Preset Updates**: Preset names sync instantly across all interfaces

### Safe Mode Recovery (v3.7+)
- **Automatic Boot Loop Detection**: System detects consecutive boot failures
- **Safe Mode**: After 3 failed boots, system starts without SDI Shield initialization
- **Diagnostic Web Interface**: Access troubleshooting page at device IP when in Safe Mode
- **Network Access Preserved**: Configure network, use vMix tally, and diagnose issues remotely
- **One-Click Recovery**: Restart in normal mode from the web interface after fixing the issue

### Preset System
- **Save/Load Presets**: Store complete camera configurations
- **Per-Camera Presets**: 5 presets per camera, stored on SD card
- **Group Selection**: Choose which parameter groups to include in presets

![Diagram](Images/Diagram.png)


---

## Hardware Requirements

| Component | Model | Notes |
|-----------|-------|-------|
| Microcontroller | **Arduino Mega 2560** | Must be Mega (not Uno) due to memory requirements |
| Network | **Arduino Ethernet Shield 2** | W5500 chipset, includes SD card slot |
| SDI Interface | **Blackmagic 3G-SDI Shield** | For Arduino, directly from Blackmagic |
| Storage | **MicroSD Card** | 1-32GB, must be formatted correctly (see below) |
| Power | **9-12V DC Power Supply** | Required - USB power alone is insufficient |

### Shield Stacking Order

```
        +---------------------+
    3   |  Blackmagic SDI     |  <- Top
        +---------------------+
    2   |  Ethernet Shield    |  <- Middle  
        +---------------------+
    1   |  Arduino Mega 2560  |  <- Bottom
        +---------------------+
```

![Arduino](Images/Arduino%20Stack%20Back.jpeg)
![Arduino](Images/Arduino%20Stack%20Front.jpeg)


### CRITICAL: I2C Bridge Wires Required

The Blackmagic SDI Shield is designed for Arduino Uno, which has I2C pins at A4/A5. The Arduino Mega has I2C on different pins (20/21), so you MUST add two jumper wires on the **Blackmagic SDI Shield**:

```
Bridge connections on SDI Shield:
  - A4 --> SDA  (Mega pin 20)
  - A5 --> SCL  (Mega pin 21)
```

On the Uno layout A4 is SDA and A5 is SCL; on the Mega, SDA is pin 20 and SCL is pin 21. Do not cross them: with SDA and SCL swapped the shield (I2C address 0x6E) does not respond and there is no tally or CCU.

These bridges route the I2C signals to the correct pins. Without them, the SDI shield will not communicate with the Arduino Mega.

### CRITICAL: External Power Required

The system requires external power (9-12V DC) connected to the Blackmagic SDI Shield's power input. USB power alone does not provide enough current and the system will fail to boot properly.

> **Note**: Starting with v3.7, if the system detects repeated boot failures (e.g., due to missing 12V power or invalid SDI signal), it will automatically enter Safe Mode. See [Safe Mode](#safe-mode) for details.

---

## SD Card Setup

### Partition and Format Requirements

The SD card MUST meet these requirements:

1. **MBR Partition Table** (not GPT) - Windows and Mac sometimes create GPT partitions
2. **Single Partition** - Only one partition on the card
3. **Correct Filesystem**:
   - Cards 2GB or smaller: FAT16
   - Cards 4GB or larger: FAT32
   - **exFAT will NOT work**

### Recommended Formatting Procedure

**Do NOT use Windows Explorer or Mac Disk Utility** - they may create incompatible partition tables.

Use the official SD Card Formatter from the SD Association:
1. Download from: https://www.sdcard.org/downloads/formatter/
2. Select your SD card
3. Choose "Overwrite format" (not quick format)
4. Format the card

If you still have issues, use the SdFormatter sketch included in the SdFat library examples.

### Copy Web Interface Files

After formatting, copy these files to the SD card root:

```
SD Card Root/
├── index.html      (Main CCU interface)
├── tally.html      (Tally configuration)
├── sdcard.html     (SD Card File manager)
└── safemode.html   (Safe Mode diagnostics - v3.7+)
```

**Optional, recommended (v3.8+): compressed pages.** Run `python3 tools/gzip_sdcard.py` and copy the generated `.gz` files next to the `.html` files (for example `index.html.gz`). The firmware serves the compressed copy to browsers that accept gzip: `index.html` goes from ~220 KB to ~41 KB, so the page loads about 5x faster and keeps the Arduino busy for much less time. Regenerate the `.gz` files every time you update the `.html` files (`python3 tools/gzip_sdcard.py --check` tells you if any is stale). When you upload, rename or delete a page through the SD Card Manager, its old `.gz` is removed automatically (the plain page is served until you copy a new `.gz`).

---

## Installation

### Step 1: Hardware Assembly

1. Stack shields in order: Mega -> Ethernet -> SDI Shield
2. **Add I2C bridge wires**: A4 to SDA (Mega pin 20), A5 to SCL (Mega pin 21)
3. Insert formatted SD card into Ethernet Shield slot
4. Connect 9-12V power supply to SDI Shield
5. Connect SDI output to your camera(s)

### Step 2: Arduino Firmware

**Required Libraries** (install via Arduino Library Manager):
- `SdFat` by Bill Greiman
- `Ethernet` (built-in)

**Blackmagic Library** (manual install):
1. Download from Blackmagic website (included with Desktop Video software)
2. Copy to Arduino libraries folder

**Upload Firmware**:
1. Open `firmware/TallyCCUPro/TallyCCUPro.ino` in Arduino IDE
2. Select Board: `Arduino Mega or Mega 2560`
3. Select Port: Your Arduino's COM port
4. Click Upload

### Step 3: Initial Network Configuration

Connect via serial monitor (115200 baud) or use the Serial Configurator tool:

![Configurator](Images/Configurator.png)


```
Available commands:
  ip 192.168.1.100        Set Arduino IP address
  subnet 255.255.255.0    Set subnet mask  
  gateway 192.168.1.1     Set gateway
  vmixip 192.168.1.50     Set vMix computer IP
  status                  Show current configuration
  reset                   Restart Arduino
  safemode                Force safe mode on next boot (v3.7+)
  normalboot              Exit safe mode and restart (v3.7+)
```

### Step 4: Verify Installation

1. Open browser: `http://YOUR_ARDUINO_IP/`
2. You should see the CCU Control interface
3. Select a camera and adjust parameters to test

---

## Web Interface

Access at `http://YOUR_ARDUINO_IP/`

> **Note**: Always use `http://` explicitly. Some modern browsers default to HTTPS, which is not supported. See [Limitations](#limitations).

### CCU Control (index.html)

The main control interface featuring:
- **Camera Selector**: Switch between cameras 1-8
- **Parameter Groups**: Lens, Video, Audio, Color Correction, Display, Tally, PTZ
- **Color Wheels**: Visual lift/gamma/gain/offset adjustment
- **Sliders**: Precise control with step buttons and reset
- **Presets**: Save and load 5 presets per camera
- **Real-Time Sync**: Changes from Companion appear instantly via SSE
- **Touch Support**: Full touch control on tablets and phones (v3.7.1+)
![Presets](Images/Presets.png)

### Tally Configuration (tally.html)

- **vMix IP**: Address of computer running vMix
- **Input Mapping**: Map vMix inputs to camera IDs
- **Connection Status**: Real-time indicator
![Tally](Images/Tally.png)

### SD Card Manager (sdcard.html)

- **Upload**: Drag and drop files
- **Download**: Backup presets
- **Delete/Rename**: File management
![Front](Images/SDCard.png)

---

## Safe Mode

Safe Mode is a recovery feature introduced in v3.7 that helps diagnose and recover from hardware-related boot failures.

### When Does Safe Mode Activate?

The system enters Safe Mode automatically after **3 consecutive boot failures**. Common causes:

1. **Missing 12V Power**: SDI Shield requires external 12V power
2. **Invalid SDI Signal**: 4K signals or Level A 3G-SDI are not supported
3. **SDI Cable Disconnected**: No signal during boot
4. **I2C Bus Conflict**: Missing or incorrect bridge wires

### What Works in Safe Mode?

| Feature | Status |
|---------|--------|
| Web Interface | ✓ Available (diagnostic page) |
| Network Configuration | ✓ Available |
| vMix Tally Connection | ✓ Receive only (no SDI output) |
| Companion TCP | ✓ Available |
| Serial Console | ✓ Available |
| **SDI Tally Output** | ✗ Disabled |
| **CCU Camera Control** | ✗ Disabled |

### Exiting Safe Mode

1. **Fix the underlying issue** (connect 12V power, valid SDI signal, etc.)
2. Access the diagnostic page at `http://YOUR_ARDUINO_IP/`
3. Click **"Restart Normal Mode"**
4. The system will restart and attempt normal boot

Alternatively, use the serial command `normalboot` to exit Safe Mode.

**Automatic recovery (v3.8+)**: in Safe Mode the firmware checks the SDI shield on I2C every 5 s. When the shield has answered for 30 s in a row (for example, the 12 V supply came back), the board restarts in normal mode by itself. If that normal boot fails again, the next automatic attempt waits 5 minutes, and after 2 failed attempts automatic recovery stays off until a manual exit or a power cycle. Note that the check only shows that the shield is powered: problems caused by the SDI input (no signal yet, 4K, Level A) are not detected beforehand.

### Manual Safe Mode Entry

For testing or troubleshooting, you can force Safe Mode on the next boot:
- Serial command: `safemode`
- The system will enter Safe Mode on the next restart

---

## Bitfocus Companion Module

TallyCCU Pro is available as an **official Bitfocus Companion module**, so you can install it directly from within Companion — no manual setup required.

### Installation

1. Open Bitfocus Companion (v4 or later)
2. Go to the **Connections** tab
3. Click **Add connection**
4. Search for **"TallyCCU Pro"** (or *Fiverecords*)
5. Add the connection and configure it with your Arduino's IP address

### Actions Available

- All CCU parameters (set/increase/decrease/reset)
- Load/Save presets
- Change active camera
- vMix connection toggle

### Real-Time Sync

The Companion module automatically syncs with the web interface:
- Parameters changed in Companion are instantly reflected in the web UI
- Parameters changed in the web UI are instantly reflected in Companion variables
- When either client connects, it receives the current state from the other

---

## TCP Protocol (Port 8098)

For custom integrations, connect via TCP to port 8098.

### Commands (send to Arduino)

```
SUBSCRIBE                       Subscribe to real-time updates
UNSUBSCRIBE                     Unsubscribe from updates
PING                            Keep-alive (responds with PONG)
STATUS                          Get connection status
```

### Events (from Arduino to subscribed clients)

```
CCU <cameraId> <paramKey> <value>           Parameter changed
PRESET <cameraId> <presetId> <name>         Preset loaded
PRESETSAVED <cameraId> <presetId> <name>    Preset saved
REQUESTSYNC                                  Request client to send cached state
PONG                                         Ping response
SUBSCRIBED OK                                Subscription confirmed
```

### Sync Commands (for bidirectional sync)

```
CCUSYNC <cameraId> <paramKey> <value>       Send cached parameter to Arduino
                                            (forwarded to SSE clients)
```

### Connection Rules (v3.8+)

- On connect the server sends `CCUSERVER OK TallyCCU-Pro`. Up to 2 clients; a third gets `ERROR Server full`.
- Lines end with `\n` (`\r\n` accepted) and may be split across TCP segments. Maximum 127 characters per line; longer lines are dropped whole and answered with `ERROR Line too long`. Unknown commands get `ERROR Unknown command`.
- A client that sends nothing within 10 s of connecting, or nothing for 90 s afterwards, is disconnected. Send `PING` periodically (Companion does it every 30 s).
- A new connection from the same IP replaces an older one from that IP only if the older one has been silent for 35 s (a stale reconnect).
- Commands are processed up to 8 per loop pass, none dropped: large `CCUSYNC` dumps are spread over several passes. `REQUESTSYNC` is sent at most every 10 s.
- Events are never allowed to block the Arduino: if a client stops reading, events for it are dropped, and a client that stays blocked for 5 s is disconnected.

---

## Architecture

### Synchronization Flow

```
┌─────────────┐         ┌─────────────┐         ┌─────────────┐
│   Web UI    │◄──SSE───│   Arduino   │◄──TCP───│  Companion  │
│  (Browser)  │───POST─►│  (Mega2560) │───TCP──►│   Module    │
└─────────────┘         └─────────────┘         └─────────────┘
      │                        │                       │
      │   cameraStates[]       │                       │   cameraStates{}
      │   (per-camera)         │                       │   (per-camera)
      └────────────────────────┴───────────────────────┘
                    Bidirectional Real-Time Sync
```

- **SSE (Server-Sent Events)**: Arduino pushes changes to web interface instantly
- **TCP Broadcast**: Arduino pushes changes to Companion module
- **POST /syncState**: Web sends cached state to Arduino for TCP broadcast
- **REQUESTSYNC**: Arduino requests state from connected clients on new connections

### Safe Mode Boot Flow (v3.7+)

```
Power On
    │
    ▼
SafeMode::begin()
    │
    ├─► Check EEPROM boot flag
    │
    ▼
┌───────────────────┐
│ Previous boot     │──NO──► Clean boot, reset counter
│ completed?        │
└────────┬──────────┘
         │YES (flag present = crash)
         ▼
    Increment counter
         │
         ▼
┌───────────────────┐
│ Counter >= 3?     │──NO──► Try normal boot
└────────┬──────────┘
         │YES
         ▼
    SAFE MODE
    - Skip SDI Shield init
    - Network/Web only
    - Serve diagnostic page
```

---

## Project Structure

```
TallyCCUPro/
├── firmware/
│   └── TallyCCUPro/      Arduino sketch (folder name must match the .ino)
│       ├── TallyCCUPro.ino   Main sketch
│       ├── CCUControl.*      Camera control
│       ├── CCUBroadcast.*    TCP broadcast server
│       ├── TallyManager.*    Tally light control
│       ├── VmixConnector.*   vMix integration
│       ├── webserver.*       HTTP & SSE server
│       ├── NetUtils.*        Non-blocking network I/O helpers (v3.8+)
│       ├── Storage.*         EEPROM management
│       ├── SdUtils.*         SD card operations
│       ├── SafeMode.*        Boot failure recovery (v3.7+)
│       ├── bmd_params.*      Blackmagic protocol definitions
│       └── Configuration.h   Pin & address definitions
├── sdcard/               Web interface files  
│   ├── index.html        Main CCU interface
│   ├── tally.html        Tally configuration
│   ├── sdcard.html       File manager
│   └── safemode.html     Safe Mode diagnostics (v3.7+)
├── tools/                Serial configurator, gzip_sdcard.py
├── docs/                 Code audit reports
└── README.md
```

---

## Memory Usage

TallyCCU Pro is optimized for Arduino Mega's limited 8KB RAM:
- Static buffers (no String objects, no heap)
- SSE-based sync (state stored in browser, not Arduino)
- Parameter table and fixed strings kept in flash (PROGMEM) since v3.8
- Static RAM in v3.8: ~3.6 KB of 8 KB (v3.7.1: ~6.1 KB), leaving about 4.5 KB for the stack; measured worst-case stack depth is about 1 KB

---

## Limitations

### HTTPS Not Supported
The Arduino Mega cannot handle HTTPS connections due to hardware limitations (insufficient RAM and CPU for TLS encryption). Always use `http://` when accessing the web interface.

> **Tip**: If your browser defaults to HTTPS and shows a connection error, manually type `http://` before the IP address.

### SDI Shield Resolution & Signal Limitations
The Blackmagic 3G-SDI Arduino Shield has the following limitations:
- **Maximum resolution**: 1080p60 (no 4K/UHD support)
- **3G-SDI Level B only**: The shield only passes video when using 3G-SDI Level B. This is the default for Blackmagic devices, but other manufacturers may default to Level A. Check your camera/device settings if video is not passing through.

### Two SSE Clients
Up to 2 browsers (tabs or devices) receive real-time sync at the same time (v3.8+). A third one replaces the oldest, which reconnects on its own. A newly opened tab may take up to 10 s to receive the current state from Companion.

### Network Connections
The W5500 (Ethernet Shield 2) has 8 sockets, shared between:
- the HTTP listener (port 80) and the TCP listener (port 8098)
- the vMix connection (tally)
- up to 2 SSE clients (browsers)
- up to 2 TCP clients on port 8098 (Companion, custom integrations)
- HTTP requests being served

Connections to port 80 that do not send a request within 5 s are closed, so idle or half-open connections cannot use up the sockets. While a slow client is downloading a page (for example a tablet on poor Wi-Fi), other HTTP requests wait until that transfer finishes or is cut (at most 10 s, about 5 s with the `.gz` pages); tally, vMix and the PTZ safety stop keep running during the transfer.

### Camera Protocol
The Blackmagic SDI protocol is **write-only** - parameters can be sent to cameras but not read back. The system cannot query current camera settings; it only knows values that were set through TallyCCU Pro.

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Web interface not loading | Verify SD card is MBR/FAT16 or FAT32, reformat with SD Association tool |
| SDI shield not responding | Check I2C bridge wires (A4→SDA / pin 20, A5→SCL / pin 21 on SDI shield) |
| System not booting | Connect external 9-12V power, USB is insufficient |
| System stuck in boot loop | Check 12V power and SDI signal; after 3 failures, Safe Mode activates automatically |
| Safe Mode activated | Fix hardware issue (12V power, valid SDI signal), then click "Restart Normal Mode" |
| vMix tally not working | Verify vMix IP in tally.html, check vMix TCP API is enabled |
| Camera not responding | Verify camera ID matches, check SDI connection and termination |
| Browser shows HTTPS error | Type `http://` explicitly before the IP address |
| Sync not working | Ensure both web and Companion are connected; check serial monitor for SSE status |
| Settings reset after reboot | Resolved in v3.5+; update firmware if running an older version |
| Controls not working on tablet | Requires v3.7.1+ which adds touch support for knobs, wheels, and joystick |

---

## Changelog

### v3.8 (Latest)
Phases 1 and 2 of the fixes from the full code audit ([docs/AUDITORIA-v3.7.1.md](docs/AUDITORIA-v3.7.1.md)). Compatible with the current Bitfocus Companion module (v3.1.0), checked with differential tests of v3.7.1 vs v3.8 driving the unmodified module.

**Live stability (phase 2)**
- **Fixed**: A browser, Companion or download client that vanished (Wi-Fi drop, laptop asleep) could freeze the Arduino or trigger a watchdog reboot on air. All network writes are now non-blocking or time-bounded, and responses are sent in blocks instead of one TCP segment per character
- **Fixed**: Tally froze while a page was served or a file uploaded; tally keeps updating during transfers (under 100 ms in tests)
- **New**: vMix heartbeat: a vMix that disappears without closing the connection is detected in ~15 s (tally off, reconnect). Reconnects retry after 2, 2, 5 and 10 s (max 30 s) instead of 20 s+
- **Fixed**: TALLY lines split across TCP packets are applied whole; the current vMix state is re-applied when the tally override or the map changes
- **New**: Tally map slots can be cleared (Clear button in tally.html)
- **Fixed**: Port 8098: proper line handling (long or split lines), large Companion sync dumps processed without blocking, idle connections reclaimed (see Connection Rules)
- **Fixed**: Companion's `timecode_source` action now reaches the camera (its `timecode_source_0` key is accepted)
- **Fixed**: Two browsers no longer kick each other off the real-time sync (2 SSE clients)
- **New**: Optional PTZ/zoom safety stop: the web interface refreshes the speed while the joystick is held, and the firmware stops the motion if refreshes stop for 600 ms (only for the web interface; Companion behaves as before)
- **New**: gzip-compressed pages (`tools/gzip_sdcard.py`), served automatically when present
- **Fixed**: Saving a preset or uploading a file is atomic: an interrupted save keeps the previous file
- **Fixed**: SD card removed or failing: errors are reported (`SD not available`) and the card is remounted automatically
- **New**: Automatic exit from Safe Mode when the shield answers again (see Safe Mode)
- **Fixed**: A stuck I2C bus no longer hangs the Arduino until the watchdog (25 ms I2C timeout)
- **Fixed**: Changing the Arduino IP reconnects vMix with the new address
- **Improved**: Web interface shows rejected commands and a connection/sync indicator, and reconnects the real-time sync if it goes silent
- **Improved**: Serial configurator no longer resets the Arduino when it connects (where the OS allows it); rebuild the `.exe` to get this
- **Improved**: Static RAM down from ~5.8 KB to ~3.6 KB (parameter table moved to flash)

**Phase 1**

**Tally / vMix**
- **Fixed**: Only vMix inputs 1-10 drove tally; any mapped input up to 63 now works
- **Fixed**: Several inputs mapped to the same camera overwrote each other; states are combined and program wins over preview (one input can also drive several cameras)
- **Fixed**: Tally now turns off when the vMix connection is lost, disabled or its IP changes (the 3.7.1 entry below claimed this, but it was not in the code)
- **Fixed**: After a refused vMix connection the firmware kept a stale socket that the web server or Companion could reuse
- **Fixed**: Debounce could leave a stale tally state after a quick B->A change
- **Improved**: vMix connect attempts time out after 250 ms instead of ~1 s, so the loop is not frozen while vMix is down

**Camera control**
- **Fixed**: Exponent values from Companion (e.g. `2.7e-17`) reached the camera as huge numbers; malformed values are now rejected
- **Fixed**: Loading a preset re-ran actions (auto white balance, autofocus, color correction reset); actions are no longer broadcast as parameter state
- **Fixed**: Boolean `true` was applied as false
- **Fixed**: Fixed-point values of 16.0 (e.g. gain adjust max) wrapped to -16.0
- **Changed**: Loading a preset no longer applies pan/tilt or zoom speed (the head could keep moving) or color bars. PTZ memory recall is still applied, but a stored "memory store" is ignored (it overwrote the memory). Video mode is still applied, except the never-set default `60,0,6,0,0` (UHD 60p) that older web presets and the Companion module store
- **Changed**: The web interface no longer stores video mode, actions, motion commands or color bars in presets

**Web server and Safe Mode**
- **Fixed**: Malformed tally map keys could write past a stack buffer
- **New**: Proper HTTP status codes: `400` for invalid parameters, `503` for camera commands in Safe Mode, `404`/`500` for presets. Saving or loading a preset no longer reports success when it fails
- **New**: `getOverrides` reports `ccuAvailable`
- **Fixed**: In Safe Mode, camera commands no longer write to the uninitialized SDI shield

**Boot and network**
- **Fixed**: Boot-loop detection now requires 20 s of stable operation, so hangs right after boot also lead to Safe Mode
- **Fixed**: Web `/reboot` and the serial `reset` command no longer count as boot failures
- **Fixed**: With a blank EEPROM, tally and CCU overrides started OFF; they now default to ON, and the default vMix IP is in the same subnet as the default Arduino IP
- **Fixed**: SPI chip selects are deselected before SD init, and SD init is retried up to 3 times
- **Improved**: Per-parameter, per-command and per-tally serial logs are off by default (`DEBUG_HOT` in `Configuration.h`); a full serial TX buffer was blocking the loop

**Web interface**
- **Fixed**: PTZ joystick and continuous zoom: one request in flight, latest value wins, and the stop command is retried, so the head no longer keeps moving after release
- **Fixed**: The joystick uses Pointer Events and always stops on release, cancel, window blur or tab switch
- **Fixed**: Synced or preset values for multi-value parameters (white balance, input levels, LUT, ND) were reverted by the next local change
- **Fixed**: A preset loaded while switching cameras was shown on the wrong camera
- **Fixed**: Preset save and load show the real result and error message; names are limited to 31 bytes
- **Fixed**: Gain adjust maximum is 15.99
- **Fixed**: Tally page accepts vMix inputs 1-63 and shows server errors

**Project**
- **Changed**: Sketch moved to `firmware/TallyCCUPro/` (Arduino requires the folder to match the `.ino` name)
- **Fixed**: `#include "WebServer.h"` did not match `webserver.h`, breaking builds on case-sensitive file systems (Linux, CI)
- **Fixed**: README I2C bridge wiring: A4 -> SDA (Mega pin 20), A5 -> SCL (Mega pin 21)

### v3.7.1
- **Fixed**: vMix reconnection loop flooding network (now uses progressive backoff)
- **Fixed**: Tally lights stuck ON after vMix disconnect
- **Fixed**: CCU Broadcast TCP buffer truncation (64→128 bytes)
- **Fixed**: Tally mappings not loading from EEPROM on page open
- **Fixed**: SD card deadlock in preset error paths
- **Fixed**: Virgin EEPROM loading invalid network config (255.255.255.255)
- **Fixed**: MAC address generation off-by-one
- **Fixed**: Safe Mode page deletable from SD card manager
- **Fixed**: Filenames with apostrophes breaking SD card manager UI
- **Fixed**: JSON filename escaping in file listing endpoint
- **New**: Touch support for knobs, color wheels, and PTZ joystick (tablet/phone)
- **New**: `getTallyMap` API endpoint for reading stored mappings
- **Improved**: Color wheel drag throttled to ~30fps
- **Improved**: Fetch error handling across entire web interface
- **Improved**: Eliminated unnecessary String allocations in serial commands
- **Improved**: Free RAM ~2,000+ bytes (up from ~1,500 in v3.5)

### v3.7
- **New**: Safe Mode recovery system for boot failures
- **New**: Automatic boot loop detection (enters Safe Mode after 3 consecutive failures)
- **New**: Diagnostic web interface (`safemode.html`) for troubleshooting
- **New**: Serial commands `safemode` and `normalboot` for manual control
- **New**: `/safemode-status` and `/safemode-exit` API endpoints
- **Improved**: TallyManager skips SDI Shield initialization in Safe Mode
- **Improved**: WebServer redirects to diagnostic page when in Safe Mode

### v3.6
- Version bump release

### v3.5
- **New**: Real-time synchronization via Server-Sent Events (SSE)
- **New**: Bidirectional sync between web interface and Companion
- **New**: Live preset name updates across all interfaces
- **Improved**: ~600 bytes RAM freed by moving state to browser
- **Improved**: Faster SSE connection establishment
- **Fixed**: Override settings (Tally/CCU/vMix) persistence bug

### v3.0
- Complete rewrite with modular architecture
- Web-based color correction with visual color wheels
- Companion module with full parameter support
- TCP broadcast for real-time updates

See [RELEASE_NOTES.md](RELEASE_NOTES.md) for full version history.

---

## Author

**Joaquin Villodre** - [github.com/fiverecords](https://github.com/fiverecords)

---

## Links

- [Blackmagic SDI Shield](https://www.blackmagicdesign.com/products/blackmagicshield)
- [Bitfocus Companion](https://bitfocus.io/companion)
- [SD Association Formatter](https://www.sdcard.org/downloads/formatter/)

## License

This project is licensed under the Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License (CC BY-NC-SA 4.0).

You are free to use, modify and redistribute this project for non-commercial purposes, provided that you give appropriate credit and keep derivatives under the same license.

## Disclaimer

This project is not affiliated with, endorsed by, or sponsored by Blackmagic Design or vMix.
Blackmagic Design and vMix are registered trademarks of their respective owners.
