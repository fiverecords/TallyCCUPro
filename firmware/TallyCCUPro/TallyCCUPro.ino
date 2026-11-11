/*
 * TallyCCUPro.ino
 * Tally and CCU System for Blackmagic cameras with vMix
 *
 * Features:
 * - Tally via SDI from vMix
 * - Full CCU control via web interface
 * - Bidirectional sync with Bitfocus Companion
 * - Preset management with SD card storage
 * - Watchdog timer for reliability
 * - Safe Mode for boot failure recovery
 *
 * Hardware required:
 * - Arduino Mega 2560
 * - Ethernet Shield W5100/W5500
 * - Blackmagic 3G-SDI Shield
 * - SD Card (FAT16/FAT32)
 *
 * Repository: https://github.com/fiverecords/TallyCCUPro
 * Copyright (c) 2026 Joaquin Villodre
 * License: CC BY-NC-SA 4.0 (Creative Commons Attribution-NonCommercial-
 * ShareAlike 4.0 International), see LICENSE
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include "Configuration.h"
#include "SafeMode.h"
#include "Storage.h"
#include "Network.h"
#include "VmixConnector.h"
#include "TallyManager.h"
#include "CCUControl.h"
#include "SdUtils.h"
#include "webserver.h"
#include "CCUBroadcast.h"

#include <avr/wdt.h>

// Loop timing
unsigned long lastWebProcess = 0;
unsigned long lastSerialProcess = 0;
unsigned long lastBroadcastProcess = 0;

// Serial command line (BOOT-19)
static char serialBuffer[SERIAL_CMD_MAX + 1];
static uint8_t serialLen = 0;
static bool serialOverflow = false;

// SD init with retries; each failed attempt can take ~2 s
bool beginSdWithRetry() {
  for (uint8_t attempt = 0; attempt < SD_INIT_ATTEMPTS; attempt++) {
    if (SdUtils::begin()) return true;
    wdt_reset();
  }
  return false;
}

void setup() {
  // Deselect every SPI slave before any SPI access (SD or Ethernet):
  // pin 10 floats until Ethernet.begin() and can corrupt SD init.
  // HIGH before OUTPUT so a CS line never glitches low.
  digitalWrite(SD_PIN_CS, HIGH);
  pinMode(SD_PIN_CS, OUTPUT);
  digitalWrite(ETH_PIN_CS, HIGH);
  pinMode(ETH_PIN_CS, OUTPUT);
  // Hardware SS (pin 53) must be an output to keep the Mega SPI master
  digitalWrite(SS, HIGH);
  pinMode(SS, OUTPUT);

  // ============================================================
  // CRITICAL: SafeMode.begin() MUST be called FIRST!
  // It enables watchdog and detects boot loops.
  // ============================================================
  Serial.begin(115200);   // the Mega's UART is ready at once: nothing to wait for

  Serial.println();
  Serial.println(F("========================================="));
  Serial.println(F("   TallyCCU Pro V" FIRMWARE_VERSION));
  Serial.println(F("========================================="));

  // Initialize safe mode detection (enables watchdog internally)
  Serial.println(F("[0/9] Initializing SafeMode..."));
  SafeMode::begin();

  // Check if we're in safe mode
  if (SafeMode::isActive()) {
    Serial.println();
    Serial.println(F("!!! SAFE MODE ACTIVE !!!"));
    Serial.println(F("SDI Shield will NOT be initialized."));
    Serial.println(F("Access web interface for diagnostics."));
    Serial.println();

    // Initialize only essential systems (skip SDI Shield)
    setupSafeMode();
    return;
  }

  // Normal boot sequence
  setupNormal();
}

// Serial report of a network result (BOOT-10). r is a NetResult.
static void printNetResult(uint8_t r, const __FlashStringHelper* what) {
  if (r == NET_INVALID) return;   // the setter explained why
  Serial.print(what);
  switch (r) {
    case NET_LINK_UP:      Serial.println(F(" changed successfully")); break;
    case NET_LINK_DOWN:    Serial.println(F(" saved and applied; no Ethernet link (cable unplugged?)")); break;
    case NET_NO_HARDWARE:  Serial.println(F(" saved; Ethernet chip not responding")); break;
    default:               Serial.println(F(" changed (link state unknown)")); break;
  }
}

// Boot-time network start (shared by both modes)
static void startNetwork() {
  NetResult nr = NetworkManager::begin();
  if (nr == NET_NO_HARDWARE) {
    Serial.println(F("ERROR: Ethernet chip not responding (check the shield)"));
  } else if (nr == NET_LINK_DOWN) {
    Serial.println(F("WARNING: no Ethernet link (cable unplugged?)"));
  }
  NetworkManager::printConfig();
}

static void printIP4(const byte ip[4]) {
  for (byte i = 0; i < 4; i++) {
    if (i) Serial.print('.');
    Serial.print(ip[i]);
  }
  Serial.println();
}

void setupNormal() {
  Serial.print(F("Firmware: "));
  Serial.println(F(FIRMWARE_VERSION));
  Serial.println();

  // Initialize EEPROM storage (layout check, settings kept)
  Serial.println(F("[1/9] Initializing EEPROM..."));
  StorageManager::begin();
  wdt_reset();

  // Initialize SD card
  Serial.println(F("[2/9] Initializing SD..."));
  if (!beginSdWithRetry()) {
    Serial.println(F("ERROR: SD not available"));
  }
  wdt_reset();

  // Initialize network
  Serial.println(F("[3/9] Initializing network..."));
  startNetwork();
  wdt_reset();

  // The first I2C access to the shield is in TallyManager::begin()
  Serial.println(F("      If system hangs at [4/9] or [5/9], check:"));
  Serial.println(F("      - 12V power to SDI Shield"));
  Serial.println(F("      - Valid SDI signal (1080p/i Level B)"));

  // Initialize Tally Manager
  Serial.println(F("[4/9] Initializing Tally (SDI Shield)..."));
  TallyManager::begin();
  wdt_reset();

  // Initialize CCU Control (THIS IS WHERE IT CAN HANG!)
  Serial.println(F("[5/9] Initializing CCU (SDI Shield)..."));
  CCUControl::begin();
  wdt_reset();

  // From here on a hung I2C transfer returns after 25 ms (with a bus reset)
  // instead of blocking until the watchdog (CCU-03). Armed after the shield
  // init so that a shield hanging there still ends in Safe Mode as before.
  if (!SafeMode::armWireTimeout()) {
    Serial.println(F("WARNING: Wire has no timeout in this core"));
  }

  // Load vMix IP from EEPROM
  Serial.println(F("[6/9] Loading vMix config..."));
  byte vmixip[4];
  StorageManager::loadVmixIP(vmixip);
  Serial.print(F("vMix IP loaded: "));
  printIP4(vmixip);
  wdt_reset();

  // Initialize vMix connection (first attempt from loop())
  Serial.println(F("[7/9] Connecting to vMix..."));
  VmixConnector::begin(vmixip);
  wdt_reset();

  // Initialize web server
  Serial.println(F("[8/9] Starting web server..."));
  WebServer::begin();
  wdt_reset();

  // Initialize CCU Broadcast server
  Serial.println(F("[9/9] Starting CCU Broadcast..."));
  CCUBroadcast::begin();
  wdt_reset();

  // Boot is marked complete from loop() after BOOT_COMPLETE_DELAY_MS

  // Show final status
  Serial.println();
  Serial.println(F("========================================="));
  Serial.println(F("   System started successfully"));
  Serial.println(F("========================================="));
  Serial.print(F("Local IP: "));
  Serial.println(NetworkManager::getLocalIP());
  Serial.print(F("vMix IP: "));
  printIP4(vmixip);
  Serial.println();
  Serial.println(F("Active ports:"));
  Serial.println(F("  - HTTP: 80"));
  Serial.println(F("  - CCU Broadcast: 8098"));
  Serial.println(F("  - vMix Tally: 8099"));
  Serial.println();
  printHelp();
  Serial.println();

  Serial.println(F("Watchdog: 8s"));
  Serial.println(F("System ready."));
  Serial.println();
}

void setupSafeMode() {
  // Minimal initialization - NO SDI Shield!

  Serial.println(F("[1/5] Initializing EEPROM..."));
  StorageManager::begin();
  wdt_reset();

  Serial.println(F("[2/5] Initializing SD..."));
  if (!beginSdWithRetry()) {
    Serial.println(F("ERROR: SD not available"));
  }
  wdt_reset();

  Serial.println(F("[3/5] Initializing network..."));
  startNetwork();
  wdt_reset();

  // Tally map and vMix connection: monitoring only, the shield is not
  // initialized, so no tally reaches the cameras
  Serial.println(F("[4/5] Initializing Tally (monitoring only)..."));
  TallyManager::begin();
  byte vmixip[4];
  StorageManager::loadVmixIP(vmixip);
  VmixConnector::begin(vmixip);
  wdt_reset();

  // Banner first: the servers start last, so nothing waits on the serial
  // output once they listen
  Serial.println();
  Serial.println(F("========================================="));
  Serial.println(F("   SAFE MODE - Limited functionality"));
  Serial.println(F("========================================="));
  Serial.print(F("Reason: "));
  Serial.println(SafeMode::getSafeModeReasonString());
  Serial.print(F("Local IP: "));
  Serial.println(NetworkManager::getLocalIP());
  Serial.println();
  Serial.println(F("Available:"));
  Serial.println(F("  - Web interface (diagnostics)"));
  Serial.println(F("  - vMix: monitoring only (NO tally output)"));
  Serial.println(F("  - Companion connection (camera commands rejected)"));
  Serial.println();
  Serial.println(F("Disabled:"));
  Serial.println(F("  - SDI tally output"));
  Serial.println(F("  - SDI Camera Control"));
  Serial.println();
  Serial.print(F("Access http://"));
  Serial.print(NetworkManager::getLocalIP());
  Serial.println(F(" for diagnostics."));
  wdt_reset();

  // Start web server (will serve safe mode page)
  Serial.println(F("[5/5] Starting web server (Safe Mode)..."));
  WebServer::begin();
  wdt_reset();

  // CCU Broadcast keeps Companion connected (camera commands are rejected)
  CCUBroadcast::begin();
  wdt_reset();

  // Don't call bootComplete() - we want to stay in safe mode
  Serial.println(F("System ready."));
}

// Low RAM warning from the stack low-water mark (MEM-02): once when it
// drops below RAM_WARN_BYTES, again only if it keeps dropping
static void checkRam(unsigned long now) {
  static unsigned long lastCheck = 0;
  static int warnedAt = 0x7FFF;
  if (now - lastCheck < RAM_CHECK_INTERVAL_MS) return;
  lastCheck = now;
  int low = SafeMode::minFreeRam();
  if (low < 0) low = SafeMode::freeRam();   // not measurable (emulator)
  if (low < RAM_WARN_BYTES && low < warnedAt - 32) {
    warnedAt = low;
    Serial.print(F("!! CRITICAL RAM: "));
    Serial.print(low);
    Serial.println(F(" bytes left at the deepest stack point"));
  }
}

void loop() {
  wdt_reset();

  unsigned long currentMillis = millis();

  // A hang early in loop() is a boot failure too: clear the crash
  // counter only after a stable period. Never in Safe Mode.
  if (currentMillis > BOOT_COMPLETE_DELAY_MS && !SafeMode::isActive()) {
    SafeMode::bootComplete();  // no-op after the first call
  }

  // Uptime; in Safe Mode, probe the SDI shield and go back to normal mode
  SafeMode::poll();

  checkRam(currentMillis);

  // PRIORITY 1: vMix Tally (every cycle) - works in safe mode too
  VmixConnector::processData();
  TallyManager::refresh();

  // PRIORITY 2: Web Server
  if (currentMillis - lastWebProcess >= WEB_PROCESS_INTERVAL) {
    lastWebProcess = currentMillis;
    WebServer::processRequests();
  }

  // PRIORITY 3: CCU Broadcast (works in safe mode for Companion)
  if (currentMillis - lastBroadcastProcess >= BROADCAST_PROCESS_INTERVAL) {
    lastBroadcastProcess = currentMillis;
    CCUBroadcast::process();
  }

  // PRIORITY 4: Serial commands
  if (currentMillis - lastSerialProcess >= SERIAL_PROCESS_INTERVAL) {
    lastSerialProcess = currentMillis;
    processSerialCommands();
  }

  // PRIORITY 5: Ethernet chip check (static IP: no DHCP maintain needed)
  NetworkManager::maintain();
}

// Lines end at CR or LF. A line longer than SERIAL_CMD_MAX is dropped
// whole, never run truncated (BOOT-19). Other control and non-ASCII bytes
// are dropped: opening the USB port without a reset (serial configurator)
// can deliver a garbage byte (0x00, 0xFF) that would otherwise spoil the
// first command, and a 0x00 would even cut it to an empty line.
void processSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (((uint8_t)c < 0x20 && c != '\n' && c != '\r' && c != '\t') || (uint8_t)c >= 0x7F) {
      continue;
    }

    if (c == '\n' || c == '\r') {
      if (serialOverflow) {
        Serial.println(F("Error: line too long (ignored)"));
      } else if (serialLen > 0) {
        serialBuffer[serialLen] = '\0';
        executeSerialCommand(serialBuffer);
      }
      serialLen = 0;
      serialOverflow = false;
    } else if (serialOverflow) {
      // discard until the end of the line
    } else if (serialLen < SERIAL_CMD_MAX) {
      serialBuffer[serialLen++] = c;
    } else {
      serialOverflow = true;
    }
  }
}

static bool cmdIs(const char* cmd, PGM_P name) {
  return strcasecmp_P(cmd, name) == 0;
}

// "aa:bb:cc:dd:ee:ff" (or '-'), hex digits in either case
static bool parseMac(const char* s, byte mac[6]) {
  for (byte i = 0; i < 6; i++) {
    byte v = 0;
    for (byte d = 0; d < 2; d++) {
      char c = *s++;
      byte h;
      if (c >= '0' && c <= '9') h = c - '0';
      else if (c >= 'a' && c <= 'f') h = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') h = c - 'A' + 10;
      else return false;
      v = (byte)(v << 4 | h);
    }
    mac[i] = v;
    if (i < 5 && *s != ':' && *s != '-') return false;
    if (i < 5) s++;
  }
  return *s == '\0';
}

static void printMac(const byte mac[6]) {
  for (byte i = 0; i < 6; i++) {
    if (mac[i] < 16) Serial.print('0');
    Serial.print(mac[i], HEX);
    if (i < 5) Serial.print(':');
  }
  Serial.println();
}

static void printRam() {
  Serial.print(F("Free RAM: "));
  Serial.print(SafeMode::freeRam());
  Serial.print(F(" bytes now, "));
  int low = SafeMode::minFreeRam();
  if (low < 0) {
    Serial.println(F("minimum n/a"));
  } else {
    Serial.print(low);
    Serial.println(F(" bytes at the deepest stack point since boot"));
  }
}

// Trimmed, case-insensitive; "command argument"
void executeSerialCommand(char* line) {
  while (*line == ' ' || *line == '\t') line++;
  char* end = line + strlen(line);
  while (end > line && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
  if (*line == '\0') return;

  char* arg = line;
  while (*arg && *arg != ' ' && *arg != '\t') arg++;
  if (*arg) {
    *arg++ = '\0';
    while (*arg == ' ' || *arg == '\t') arg++;
  }

  Serial.print(F("Command: "));
  Serial.print(line);
  if (*arg) {
    Serial.print(' ');
    Serial.print(arg);
  }
  Serial.println();

  // Commands with an IP argument
  if (cmdIs(line, PSTR("ip")) || cmdIs(line, PSTR("gateway")) ||
      cmdIs(line, PSTR("subnet")) || cmdIs(line, PSTR("vmixip"))) {
    if (*arg == '\0') {
      Serial.print(F("Usage: "));
      Serial.print(line);
      Serial.println(F(" X.X.X.X"));
    } else if (cmdIs(line, PSTR("ip"))) {
      printNetResult(NetworkManager::setLocalIP(arg), F("IP"));
    } else if (cmdIs(line, PSTR("gateway"))) {
      printNetResult(NetworkManager::setGateway(arg), F("Gateway"));
    } else if (cmdIs(line, PSTR("subnet"))) {
      printNetResult(NetworkManager::setSubnet(arg), F("Subnet"));
    } else {
      changeVMixIP(arg);
    }
    return;
  }

  if (cmdIs(line, PSTR("mac"))) {
    byte mac[6];
    if (*arg == '\0') {
      NetworkManager::getMACAddress(mac);
      Serial.print(F("MAC: "));
      printMac(mac);
    } else if (!parseMac(arg, mac) || !StorageManager::saveMacAddress(mac)) {
      Serial.println(F("Error: use a unicast MAC like 02:54:00:00:00:01"));
    } else {
      Serial.print(F("MAC saved, applies after 'reset': "));
      printMac(mac);
    }
    return;
  }

  if (cmdIs(line, PSTR("factoryreset"))) {
    if (!cmdIs(arg, PSTR("yes"))) {
      Serial.println(F("Erases IP, gateway, subnet, vMix IP, tally map and overrides"));
      Serial.println(F("(the MAC is kept) and restarts. Type 'factoryreset yes'."));
      return;
    }
    StorageManager::factoryReset();
    Serial.println(F("Restarting..."));
    SafeMode::restart();
    return;
  }

  // The rest take no argument
  if (*arg) {
    Serial.println(F("Unknown command. Type 'help' for available commands."));
    return;
  }

  if (cmdIs(line, PSTR("help")) || cmdIs(line, PSTR("?"))) {
    printHelp();
    return;
  }

  // Safe mode commands
  if (cmdIs(line, PSTR("safemode"))) {
    if (SafeMode::isActive()) {
      Serial.println(F("Already in Safe Mode"));
      return;
    }
    Serial.println(F("Restarting NOW into Safe Mode (no SDI tally or CCU until 'normalboot')..."));
    SafeMode::enterSafeMode();
    return;
  }

  if (cmdIs(line, PSTR("normalboot"))) {
    if (!SafeMode::isActive()) {
      Serial.println(F("Not in Safe Mode (use 'reset' to restart)"));
      return;
    }
    Serial.println(F("Forcing normal boot..."));
    SafeMode::exitSafeMode();
    return;
  }

  if (cmdIs(line, PSTR("reset"))) {
    Serial.println(F("Resetting..."));
    SafeMode::restart();  // intentional: not counted as a boot failure
    return;
  }

  if (cmdIs(line, PSTR("status"))) {
    printStatus();
    return;
  }

  if (cmdIs(line, PSTR("ram"))) {
    printRam();
    return;
  }

  if (cmdIs(line, PSTR("tally"))) {
    TallyManager::printCurrentMap();
    return;
  }

  if (cmdIs(line, PSTR("state"))) {
    Serial.println(F("SSE active - state lives in browser"));
    Serial.print(F("SSE client connected: "));
    Serial.println(WebServer::hasSSEClient() ? F("Yes") : F("No"));
    return;
  }

  Serial.println(F("Unknown command. Type 'help' for available commands."));
}

void printHelp() {
  Serial.println(F("Available serial commands:"));
  Serial.println(F("  help            - Show this help"));
  Serial.println(F("  status          - Show system status"));
  Serial.println(F("  ip X.X.X.X      - Change local IP"));
  Serial.println(F("  gateway X.X.X.X - Change gateway (0.0.0.0: none)"));
  Serial.println(F("  subnet X.X.X.X  - Change subnet"));
  Serial.println(F("  vmixip X.X.X.X  - Change vMix IP"));
  Serial.println(F("  mac [XX:..:XX]  - Show or set the MAC (after reset)"));
  Serial.println(F("  tally           - Show tally map"));
  Serial.println(F("  state           - Show real-time sync (SSE) state"));
  Serial.println(F("  ram             - Show free RAM"));
  Serial.println(F("  reset           - Reset Arduino"));
  Serial.println(F("  safemode        - Restart NOW into Safe Mode"));
  Serial.println(F("  normalboot      - Leave Safe Mode (restarts)"));
  Serial.println(F("  factoryreset    - Factory settings (asks to confirm)"));
}

void printStatus() {
  Serial.println();
  Serial.println(F("=== SYSTEM STATUS ==="));
  Serial.print(F("Firmware: "));
  Serial.println(F(FIRMWARE_VERSION));

  // Safe mode status
  Serial.print(F("Safe Mode: "));
  if (SafeMode::isActive()) {
    Serial.print(F("ACTIVE ("));
    Serial.print(SafeMode::getSafeModeReasonString());
    Serial.println(')');
  } else {
    Serial.println(F("OFF"));
  }
  Serial.print(F("Reset Count: "));
  Serial.println(SafeMode::getResetCount());
  Serial.print(F("Last Reset: "));
  Serial.println(SafeMode::getResetReasonString());
  Serial.print(F("Watchdog resets since power-on: "));
  Serial.println(SafeMode::getCrashCount());
  uint32_t hang = SafeMode::getCrashAddress();
  if (hang) {
    Serial.print(F("Last hang at flash address: 0x"));
    Serial.println(hang, HEX);
  }
  Serial.print(F("Uptime: "));
  Serial.print(SafeMode::getUptime());
  Serial.println(F(" seconds"));
  if (SafeMode::isActive()) {
    Serial.print(F("Auto-recovery: "));
    Serial.print(SafeMode::autoRecoveryArmed() ? F("ON") : F("OFF"));
    Serial.print(F(" (strikes "));
    Serial.print(SafeMode::getRecoveryStrikes());
    Serial.println(')');
  }
  Serial.println();

  Serial.print(F("Local IP: "));
  Serial.println(NetworkManager::getLocalIP());
  Serial.print(F("Gateway: "));
  Serial.println(NetworkManager::getGateway());
  Serial.print(F("Subnet: "));
  Serial.println(NetworkManager::getSubnet());
  byte mac[6];
  NetworkManager::getMACAddress(mac);
  Serial.print(F("MAC: "));
  printMac(mac);
  Serial.print(F("Ethernet: "));
  switch (NetworkManager::linkState()) {
    case NET_LINK_UP:     Serial.println(F("link up")); break;
    case NET_LINK_DOWN:   Serial.println(F("no link")); break;
    case NET_NO_HARDWARE: Serial.println(F("chip not responding")); break;
    default:              Serial.println(F("link state unknown")); break;
  }

  byte vmixip[4];
  VmixConnector::getVmixIP(vmixip);
  Serial.print(F("vMix IP: "));
  printIP4(vmixip);
  Serial.print(F("vMix enabled: "));
  Serial.println(VmixConnector::getConnectEnabled() ? F("YES") : F("NO"));
  Serial.print(F("vMix connected: "));
  Serial.println(VmixConnector::isConnected() ? F("YES") : F("NO"));

  Serial.print(F("Tally override: "));
  Serial.println(TallyManager::getOverride() ? F("ON") : F("OFF"));

  if (!SafeMode::isActive()) {
    Serial.print(F("CCU override: "));
    Serial.println(CCUControl::getOverride() ? F("ON") : F("OFF"));
    Serial.print(F("Active camera: "));
    Serial.println(CCUControl::getActiveCamera());
  } else {
    Serial.println(F("CCU: DISABLED (Safe Mode)"));
  }

  Serial.print(F("CCU Broadcast clients: "));
  Serial.println(CCUBroadcast::getClientCount());

  Serial.print(F("SSE client: "));
  Serial.println(WebServer::hasSSEClient() ? F("Yes") : F("No"));

  printRam();

  Serial.println(F("====================="));
  Serial.println();
}

// Serial "vmixip" and web /?vmixIP= (declared in webserver.h)
void changeVMixIP(const char* ip) {
  byte vmixip[4];
  if (!NetworkManager::parseIP(ip, vmixip)) {
    Serial.println(F("Error: Invalid IP format"));
    return;
  }
  // Validates, writes the EEPROM and logs; explains a rejection
  VmixConnector::setVmixIP(vmixip);
}
