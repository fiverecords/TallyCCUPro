/*
 * Configuration.h
 * Central configuration file for TallyCCU Pro
 * Version 3.6
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
#define FIRMWARE_VERSION "3.6"

// ============================================================
// NETWORK CONFIGURATION
// ============================================================
#define VMIX_PORT 8099            // vMix Tally port
#define HTTP_PORT 80              // Web server port

// ============================================================
// TALLY CONFIGURATION
// ============================================================
#define MAXTALLIES 10             // Max mapped tally inputs
#define MAX_CAMERAS 8             // Max supported cameras

// ============================================================
// EEPROM ADDRESSES
// ============================================================
#define EEPROM_IP_START      40   // 4 bytes for local IP
#define EEPROM_GATEWAY_START 44   // 4 bytes for gateway
#define EEPROM_SUBNET_START  48   // 4 bytes for subnet mask
#define MAC_BYTE_5_ADDRESS   30   // For pseudo-random MAC
#define MAC_BYTE_6_ADDRESS   31   // For pseudo-random MAC
#define EEPROM_TALLY_MAP_START 100  // Tally map start (inputs)
#define EEPROM_CAMERA_MAP_START 200 // Camera map start (cams)
#define EEPROM_TALLY_OVERRIDE_ADDR  300 // Tally override (1 byte)
#define EEPROM_CCU_OVERRIDE_ADDR    301 // CCU override (1 byte)
#define EEPROM_VMIX_CONNECT_ADDR    302 // vMix connection state (1 byte)

// ============================================================
// SD CARD CONFIGURATION
// ============================================================
#define SD_PIN_CS 4               // CS pin for SD card
#define SD_BUFFER_SIZE 256        // Buffer size for SD operations

// ============================================================
// TIMEOUTS AND RETRY
// ============================================================
#define HTTP_REQUEST_TIMEOUT 5000   // HTTP request timeout (ms)
#define VMIX_RECONNECT_DELAY 20000  // vMix reconnection delay (ms)

// ============================================================
// PRESET LIMITS
// ============================================================
#define MAX_PRESETS 5             // Max presets per camera
#define MAX_PRESET_SIZE 1024      // Max preset size in bytes
#define MAX_PRESET_NAME_LENGTH 32 // Max preset name length

// ============================================================
// I2C - BLACKMAGIC SHIELDS
// ============================================================
#define BMD_I2C_ADDRESS 0x6E

// ============================================================
// LOOP TIMING CONFIGURATION
// ============================================================
#define WEB_PROCESS_INTERVAL 10      // ms between web processing
#define SERIAL_PROCESS_INTERVAL 50   // ms between serial processing
#define MAINTENANCE_INTERVAL 100     // ms between Ethernet maintenance

// ============================================================
// CONCURRENCY CONTROL VARIABLES
// ============================================================
extern volatile bool g_isBusyWithSD;
extern volatile bool g_isBusyWithNetwork;

#endif // CONFIGURATION_H
