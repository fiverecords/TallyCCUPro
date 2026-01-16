# TallyCCU Pro

<p align="center">
  <strong>Open-source CCU control system for Blackmagic Design cameras with vMix tally integration using the Blackmagic Arduino SDI Shield,</strong>
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

### Preset System
- **Save/Load Presets**: Store complete camera configurations
- **Per-Camera Presets**: 5 presets per camera, stored on SD card
- **Group Selection**: Choose which parameter groups to include in presets

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

### CRITICAL: I2C Bridge Wires Required

The Blackmagic SDI Shield is designed for Arduino Uno, which has I2C pins at A4/A5. The Arduino Mega has I2C on different pins (20/21), so you MUST add two jumper wires on the **Blackmagic SDI Shield**:

```
Bridge connections on SDI Shield:
  - A4 --> SCL
  - A5 --> SDA
```

These bridges route the I2C signals to the correct pins. Without them, the SDI shield will not communicate with the Arduino Mega.

### CRITICAL: External Power Required

The system requires external power (9-12V DC) connected to the Blackmagic SDI Shield's power input. USB power alone does not provide enough current and the system will fail to boot properly.

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
+-- index.html      (124 KB - Main CCU interface)
+-- tally.html      (11 KB - Tally configuration)
+-- sdcard.html     (18 KB - File manager)
```

---

## Installation

### Step 1: Hardware Assembly

1. Stack shields in order: Mega -> Ethernet -> SDI Shield
2. **Add I2C bridge wires**: A4 to pin 20, A5 to pin 21
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
1. Open `Arduino/TallyCCUPro.ino` in Arduino IDE
2. Select Board: `Arduino Mega or Mega 2560`
3. Select Port: Your Arduino's COM port
4. Click Upload

### Step 3: Initial Network Configuration

Connect via serial monitor (115200 baud) or use the Serial Configurator tool:

```
Available commands:
  ip 192.168.1.100        Set Arduino IP address
  subnet 255.255.255.0    Set subnet mask  
  gateway 192.168.1.1     Set gateway
  vmixip 192.168.1.50     Set vMix computer IP
  status                  Show current configuration
  reset                   Restart Arduino
```

### Step 4: Verify Installation

1. Open browser: `http://YOUR_ARDUINO_IP/`
2. You should see the CCU Control interface
3. Select a camera and adjust tally brightness to test

---

## Web Interface

Access at `http://YOUR_ARDUINO_IP/`

### CCU Control (index.html)

The main control interface featuring:
- **Camera Selector**: Switch between cameras 1-8
- **Parameter Groups**: Lens, Video, Audio, Color Correction, Display, Tally, PTZ
- **Color Wheels**: Visual lift/gamma/gain/offset adjustment
- **Sliders**: Precise control with step buttons and reset
- **Presets**: Save and load 5 presets per camera

### Tally Configuration (tally.html)

- **vMix IP**: Address of computer running vMix
- **Input Mapping**: Map vMix inputs to camera IDs
- **Connection Status**: Real-time indicator

### SD Card Manager (sdcard.html)

- **Upload**: Drag and drop files
- **Download**: Backup presets
- **Delete/Rename**: File management

---

## Bitfocus Companion Module

### Installation

1. Copy `companion-module/` to Companion's dev modules directory
2. Run `yarn install` inside the folder
3. In Companion Settings, set developer modules path
4. Add connection: "TallyCCU Pro"

### Actions Available

- All CCU parameters (set/increase/decrease/reset)
- Load/Save presets
- Change active camera
- vMix connection toggle

---

## TCP Protocol (Port 8098)

For custom integrations, connect via TCP to port 8098.

### Commands (send to Arduino)

```
CAM:X                           Select camera (X = 1-8)
PARAM:key=value                 Set parameter
PRESET:LOAD:cameraId,presetId   Load preset
PRESET:SAVE:cameraId,presetId   Save preset
PING                            Keep-alive
```

### Events (from Arduino)

```
CAM:X                           Camera changed
PARAM:cameraId:key=value        Parameter updated
TALLY:cameraId:state            Tally changed (P/V/O)
PRESETSAVED:cameraId,presetId,name   Preset saved
PONG                            Ping response
```

---

## Project Structure

```
TallyCCUPro/
|-- Arduino/               Arduino source code
|-- sdcard/                 Web interface files  
|-- companion-module/       Bitfocus Companion module
|-- tools/                  Serial configurator
+-- README.md
```

---

## Memory Usage

TallyCCU Pro is optimized for Arduino Mega's limited 8KB RAM:
- Static buffers (no String objects)
- Parameter caching
- Efficient TCP handling
- Typical free RAM: ~900 bytes

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Web interface not loading | Verify SD card is MBR/FAT16 or FAT32, reformat with SD Association tool |
| SDI shield not responding | Check I2C bridge wires (A4-20, A5-21) |
| System not booting | Connect external 9-12V power, USB is insufficient |
| vMix tally not working | Verify vMix IP in tally.html, check vMix TCP API enabled |
| Camera not responding | Verify camera ID, check SDI connection |

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

