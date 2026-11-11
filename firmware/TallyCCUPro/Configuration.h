/*
 * Configuration.h
 * Central configuration file for TallyCCU Pro
 */

#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EEPROM.h>
#include <Wire.h>
#include <SdFat.h>
#include <BMDSDIControl.h>

// ============================================================
// FIRMWARE VERSION
// ============================================================
#define FIRMWARE_VERSION "3.8.2"

// ============================================================
// NETWORK CONFIGURATION
// ============================================================
#define VMIX_PORT 8099            // vMix Tally port
#define HTTP_PORT 80              // Web server port

// ============================================================
// TALLY CONFIGURATION
// ============================================================
#define MAXTALLIES 10             // Number of tally mapping slots (input->camera pairs)
#define MAX_CAMERAS 8             // Max supported cameras
#define MAX_VMIX_INPUTS 63        // Highest vMix input number evaluated for tally
                                  // (web UI and tally map accept 1-63)

// ============================================================
// EEPROM LAYOUT (BOOT-16)
// ============================================================
// Single map of every EEPROM cell. Addresses 20-51 and 100-316 are those of
// v3.7.1/v3.8 and must not move: units in the field keep their settings
// across updates. Layout version 1 = that map plus the header at 0.
#define EEPROM_HEADER_ADDR        0   // magic (2), layout version, ~version
#define EEPROM_HEADER_SIZE        4
#define EEPROM_MAGIC0             0x54  // 'T'
#define EEPROM_MAGIC1             0x43  // 'C'
#define EEPROM_LAYOUT_VERSION     1

// vMix IP EEPROM addresses (2 bytes each via writeInt, high byte 0)
#define EEPROM_VMIX_IP0  20
#define EEPROM_VMIX_IP1  22
#define EEPROM_VMIX_IP2  24
#define EEPROM_VMIX_IP3  26

#define MAC_BYTE_5_ADDRESS   30   // legacy MAC A8:61:0A:AE:<30>:<31>
#define MAC_BYTE_6_ADDRESS   31
#define EEPROM_IP_START      40   // 4 bytes for local IP
#define EEPROM_GATEWAY_START 44   // 4 bytes for gateway
#define EEPROM_SUBNET_START  48   // 4 bytes for subnet mask
#define EEPROM_MAC_MODE_ADDR 52   // EEPROM_MAC_FULL: MAC below; else legacy MAC
#define EEPROM_MAC_START     53   // 6 bytes (BOOT-15)
#define EEPROM_MAC_FULL      0x01
#define EEPROM_TALLY_MAP_START 100  // Tally map start (inputs, 2 bytes each)
#define EEPROM_CAMERA_MAP_START 200 // Camera map start (cams, 2 bytes each)
#define EEPROM_TALLY_OVERRIDE_ADDR  300 // Tally override (1 byte)
#define EEPROM_CCU_OVERRIDE_ADDR    301 // CCU override (1 byte)
#define EEPROM_VMIX_CONNECT_ADDR    302 // vMix connection state (1 byte)

// Safe Mode (SafeMode.cpp)
#define EEPROM_RESET_COUNT_ADDR      310  // legacy boot-failure counter
#define EEPROM_RESET_FLAG_ADDR       311  // legacy "boot in progress" flag (2 bytes)
#define EEPROM_RESET_REASON_ADDR     313  // reserved (never written)
#define EEPROM_SAFEMODE_MANUAL_ADDR  314  // Safe Mode requested (0xAA)
#define EEPROM_SAFEMODE_STRIKES_ADDR 315  // failed auto-recoveries
#define EEPROM_SAFEMODE_AUTO_ADDR    316  // boot started by an auto-recovery
#define EEPROM_BOOTREC_OK_ADDR       317  // RAM kept across resets (verified)
#define EEPROM_USED_END              318

// [a, a+n) must end before b
#define EEPROM_BEFORE(a, n, b) static_assert((a) + (n) <= (b), "EEPROM map overlap at " #a)
EEPROM_BEFORE(EEPROM_HEADER_ADDR, EEPROM_HEADER_SIZE, EEPROM_VMIX_IP0);
EEPROM_BEFORE(EEPROM_VMIX_IP0, 2, EEPROM_VMIX_IP1);
EEPROM_BEFORE(EEPROM_VMIX_IP1, 2, EEPROM_VMIX_IP2);
EEPROM_BEFORE(EEPROM_VMIX_IP2, 2, EEPROM_VMIX_IP3);
EEPROM_BEFORE(EEPROM_VMIX_IP3, 2, MAC_BYTE_5_ADDRESS);
EEPROM_BEFORE(MAC_BYTE_5_ADDRESS, 1, MAC_BYTE_6_ADDRESS);
EEPROM_BEFORE(MAC_BYTE_6_ADDRESS, 1, EEPROM_IP_START);
EEPROM_BEFORE(EEPROM_IP_START, 4, EEPROM_GATEWAY_START);
EEPROM_BEFORE(EEPROM_GATEWAY_START, 4, EEPROM_SUBNET_START);
EEPROM_BEFORE(EEPROM_SUBNET_START, 4, EEPROM_MAC_MODE_ADDR);
EEPROM_BEFORE(EEPROM_MAC_MODE_ADDR, 1, EEPROM_MAC_START);
EEPROM_BEFORE(EEPROM_MAC_START, 6, EEPROM_TALLY_MAP_START);
EEPROM_BEFORE(EEPROM_TALLY_MAP_START, 2 * MAXTALLIES, EEPROM_CAMERA_MAP_START);
EEPROM_BEFORE(EEPROM_CAMERA_MAP_START, 2 * MAXTALLIES, EEPROM_TALLY_OVERRIDE_ADDR);
EEPROM_BEFORE(EEPROM_TALLY_OVERRIDE_ADDR, 1, EEPROM_CCU_OVERRIDE_ADDR);
EEPROM_BEFORE(EEPROM_CCU_OVERRIDE_ADDR, 1, EEPROM_VMIX_CONNECT_ADDR);
EEPROM_BEFORE(EEPROM_VMIX_CONNECT_ADDR, 1, EEPROM_RESET_COUNT_ADDR);
EEPROM_BEFORE(EEPROM_RESET_COUNT_ADDR, 1, EEPROM_RESET_FLAG_ADDR);
EEPROM_BEFORE(EEPROM_RESET_FLAG_ADDR, 2, EEPROM_RESET_REASON_ADDR);
EEPROM_BEFORE(EEPROM_RESET_REASON_ADDR, 1, EEPROM_SAFEMODE_MANUAL_ADDR);
EEPROM_BEFORE(EEPROM_SAFEMODE_MANUAL_ADDR, 1, EEPROM_SAFEMODE_STRIKES_ADDR);
EEPROM_BEFORE(EEPROM_SAFEMODE_STRIKES_ADDR, 1, EEPROM_SAFEMODE_AUTO_ADDR);
EEPROM_BEFORE(EEPROM_SAFEMODE_AUTO_ADDR, 1, EEPROM_BOOTREC_OK_ADDR);
EEPROM_BEFORE(EEPROM_BOOTREC_OK_ADDR, 1, EEPROM_USED_END);
EEPROM_BEFORE(EEPROM_USED_END, 0, E2END + 1);

// Factory defaults (blank EEPROM, factoryreset). vMix in the same /24.
#define DEFAULT_LOCAL_IP   192, 168, 0, 100
#define DEFAULT_GATEWAY    192, 168, 0, 1
#define DEFAULT_SUBNET     255, 255, 255, 0
#define DEFAULT_VMIX_IP    192, 168, 0, 140

// ============================================================
// SD CARD CONFIGURATION
// ============================================================
#define SD_PIN_CS 4               // CS pin for SD card
#define ETH_PIN_CS 10             // CS pin for W5500 (Ethernet Shield 2)
#define SD_BUFFER_SIZE 256        // Buffer size for SD operations
#define SD_INIT_ATTEMPTS 3        // SD begin() attempts at boot

// ============================================================
// SERIAL CONSOLE
// ============================================================
#define SERIAL_CMD_MAX 31         // longest command line (BOOT-19)

// ============================================================
// TIMEOUTS AND RETRY
// ============================================================
#define VMIX_HEARTBEAT_MS 5000      // idle time before a TALLY request to vMix
#define VMIX_RX_TIMEOUT_MS 15000    // nothing from vMix this long = dead link
#define VMIX_RECONNECT_MAX_MS 30000 // longest wait between vMix connect attempts

// ============================================================
// PRESET LIMITS
// ============================================================
#define MAX_PRESETS 5             // Max presets per camera
#define MAX_PRESET_NAME_LENGTH 32 // Max preset name length

// ============================================================
// I2C - BLACKMAGIC SHIELDS
// ============================================================
#define BMD_I2C_ADDRESS 0x6E

// ============================================================
// LOOP TIMING CONFIGURATION
// ============================================================
#define WEB_PROCESS_INTERVAL 10      // ms between web processing
#define SERIAL_PROCESS_INTERVAL 5    // ms between serial reads (64-byte RX ring = 5.5 ms at 115200)
#define BROADCAST_PROCESS_INTERVAL 20 // ms between CCU Broadcast (8098) passes
#define NET_CHECK_INTERVAL_MS 5000   // W5500 settings check (BOOT-12)
#define RAM_CHECK_INTERVAL_MS 10000  // low RAM warning check (MEM-02)
#define RAM_WARN_BYTES 300           // warn when the stack came this close

// Tally refresh (TAL-12): every TALLY_REFRESH_MS the next camera slot is
// visited (a round of 8 takes 4 s): a mapped camera, or one whose last write
// failed, gets its current tally again, and once per round the override is
// read back, so a lost I2C write or a shield reset heals itself. One write
// at a time keeps the I2C time per loop pass small. 0 disables it.
#ifndef TALLY_REFRESH_MS
#define TALLY_REFRESH_MS 500
#endif

// ============================================================
// DEBUG LOGGING
// ============================================================
// Verbose serial logging on hot paths (every CCU parameter, every TCP
// command, every tally change). Disabled by default: once the 64-byte
// Serial TX buffer fills, Serial.print() blocks the main loop
// (~87 us per byte at 115200 baud) and delays tally. Set to 1 to debug.
#ifndef DEBUG_HOT
#define DEBUG_HOT 0
#endif

#endif // CONFIGURATION_H
