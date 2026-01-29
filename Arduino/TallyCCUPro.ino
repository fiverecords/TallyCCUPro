/*
 * TallyCCUPro.ino
 * Tally and CCU System for Blackmagic cameras with vMix
 * 
 * Version 3.7
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
 * Repository: https://github.com/YOUR_USERNAME/TallyCCUPro
 * License: GPL-3.0
 */

#include "Configuration.h"
#include "SafeMode.h"       // NEW: Safe mode system
#include "Storage.h"
#include "Network.h"
#include "VmixConnector.h"
#include "TallyManager.h"
#include "CCUControl.h"
#include "SdUtils.h"
#include "WebServer.h"
#include "CCUBroadcast.h"

#include <avr/wdt.h>

// Concurrency control flags
volatile bool g_isBusyWithSD = false;
volatile bool g_isBusyWithNetwork = false;

// EEPROM addresses for vMix IP
const int ADDRVMIX0 = 20;
const int ADDRVMIX1 = 22;
const int ADDRVMIX2 = 24;
const int ADDRVMIX3 = 26;

// Loop timing
unsigned long lastWebProcess = 0;
unsigned long lastSerialProcess = 0;
unsigned long lastMaintenance = 0;
unsigned long lastBroadcastProcess = 0;

// Serial command buffer
char serialBuffer[64];
int serialBufferIndex = 0;

void setup() {
  // ============================================================
  // CRITICAL: SafeMode.begin() MUST be called FIRST!
  // It enables watchdog and detects boot loops.
  // ============================================================
  Serial.begin(115200);
  while (!Serial && millis() < 3000);
  
  Serial.println();
  Serial.println(F("========================================="));
  Serial.println(F("   TallyCCU Pro V3.7"));
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

void setupNormal() {
  Serial.print(F("Firmware: "));
  Serial.println(FIRMWARE_VERSION);
  Serial.println();

  // Initialize EEPROM storage
  Serial.println(F("[1/9] Initializing EEPROM..."));
  StorageManager::begin();
  wdt_reset();
  
  // Initialize SD card
  Serial.println(F("[2/9] Initializing SD..."));
  if (!SdUtils::begin()) {
    Serial.println(F("ERROR: SD not available"));
  }
  wdt_reset();
  
  // Initialize network
  Serial.println(F("[3/9] Initializing network..."));
  if (!NetworkManager::begin()) {
    Serial.println(F("ERROR: Network not available"));
  }
  NetworkManager::printConfig();
  wdt_reset();
  
  // Initialize Tally Manager
  Serial.println(F("[4/9] Initializing Tally..."));
  TallyManager::begin();
  wdt_reset();
  
  // Initialize CCU Control (THIS IS WHERE IT CAN HANG!)
  Serial.println(F("[5/9] Initializing CCU (SDI Shield)..."));
  Serial.println(F("      If system hangs here, check:"));
  Serial.println(F("      - 12V power to SDI Shield"));
  Serial.println(F("      - Valid SDI signal (1080p/i Level B)"));
  CCUControl::begin();
  wdt_reset();
  
  // Load vMix IP from EEPROM
  Serial.println(F("[6/9] Loading vMix config..."));
  byte vmixip[4];
  loadVMixIPFromEEPROM(vmixip);
  wdt_reset();
  
  // Initialize vMix connection
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
  
  // Mark boot as complete - this clears the crash counter
  SafeMode::bootComplete();
  
  // Show final status
  Serial.println();
  Serial.println(F("========================================="));
  Serial.println(F("   System started successfully"));
  Serial.println(F("========================================="));
  Serial.print(F("Local IP: "));
  Serial.println(NetworkManager::getLocalIP());
  Serial.print(F("vMix IP: "));
  Serial.print(vmixip[0]); Serial.print('.');
  Serial.print(vmixip[1]); Serial.print('.');
  Serial.print(vmixip[2]); Serial.print('.');
  Serial.println(vmixip[3]);
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
  if (!SdUtils::begin()) {
    Serial.println(F("ERROR: SD not available"));
  }
  wdt_reset();
  
  Serial.println(F("[3/5] Initializing network..."));
  if (!NetworkManager::begin()) {
    Serial.println(F("ERROR: Network not available"));
  }
  NetworkManager::printConfig();
  wdt_reset();
  
  // Initialize Tally Manager (for vMix connection)
  Serial.println(F("[4/5] Initializing Tally..."));
  TallyManager::begin();
  wdt_reset();
  
  // Load vMix IP and connect (tally still works!)
  byte vmixip[4];
  loadVMixIPFromEEPROM(vmixip);
  VmixConnector::begin(vmixip);
  wdt_reset();
  
  // Start web server (will serve safe mode page)
  Serial.println(F("[5/5] Starting web server (Safe Mode)..."));
  WebServer::begin();
  wdt_reset();
  
  // CCU Broadcast still works (for Companion)
  CCUBroadcast::begin();
  wdt_reset();
  
  // Don't call bootComplete() - we want to stay in safe mode
  
  Serial.println();
  Serial.println(F("========================================="));
  Serial.println(F("   SAFE MODE - Limited functionality"));
  Serial.println(F("========================================="));
  Serial.print(F("Local IP: "));
  Serial.println(NetworkManager::getLocalIP());
  Serial.println();
  Serial.println(F("Available:"));
  Serial.println(F("  - Web interface (diagnostics)"));
  Serial.println(F("  - vMix tally connection"));
  Serial.println(F("  - Companion integration"));
  Serial.println();
  Serial.println(F("Disabled:"));
  Serial.println(F("  - SDI Camera Control"));
  Serial.println();
  Serial.println(F("Access http://"));
  Serial.print(NetworkManager::getLocalIP());
  Serial.println(F(" for diagnostics."));
  Serial.println();
}

void loop() {
  wdt_reset();
  
  unsigned long currentMillis = millis();
  
  // RAM monitor (every 10 seconds, only warn if critical)
  static unsigned long lastRamCheck = 0;
  if (currentMillis - lastRamCheck >= 10000) {
    lastRamCheck = currentMillis;
    int ram = freeRam();
    if (ram < 300) {
      Serial.print(F("!! CRITICAL RAM: "));
      Serial.println(ram);
    }
  }
  
  // PRIORITY 1: vMix Tally (every cycle) - works in safe mode too
  VmixConnector::processData();
  
  // PRIORITY 2: Web Server
  if (currentMillis - lastWebProcess >= WEB_PROCESS_INTERVAL) {
    lastWebProcess = currentMillis;
    if (!g_isBusyWithSD) {
      WebServer::processRequests();
    }
  }
  
  // PRIORITY 3: CCU Broadcast (works in safe mode for Companion)
  if (currentMillis - lastBroadcastProcess >= 20) {
    lastBroadcastProcess = currentMillis;
    CCUBroadcast::process();
  }
  
  // PRIORITY 4: Serial commands
  if (currentMillis - lastSerialProcess >= SERIAL_PROCESS_INTERVAL) {
    lastSerialProcess = currentMillis;
    processSerialCommands();
  }
  
  // PRIORITY 5: Ethernet maintenance
  if (currentMillis - lastMaintenance >= MAINTENANCE_INTERVAL) {
    lastMaintenance = currentMillis;
    Ethernet.maintain();
  }
}

void processSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    
    if (c == '\n' || c == '\r') {
      if (serialBufferIndex > 0) {
        serialBuffer[serialBufferIndex] = '\0';
        executeSerialCommand(serialBuffer);
        serialBufferIndex = 0;
      }
    } else if (serialBufferIndex < 63) {
      serialBuffer[serialBufferIndex++] = c;
    }
  }
}

void executeSerialCommand(const char* command) {
  Serial.print(F("Command: "));
  Serial.println(command);
  
  if (strcmp(command, "help") == 0 || strcmp(command, "?") == 0) {
    printHelp();
    return;
  }
  
  // Safe mode commands
  if (strcmp(command, "safemode") == 0) {
    Serial.println(F("Entering safe mode on next boot..."));
    SafeMode::enterSafeMode();
    return;
  }
  
  if (strcmp(command, "normalboot") == 0) {
    Serial.println(F("Forcing normal boot..."));
    SafeMode::exitSafeMode();
    return;
  }
  
  if (strncmp(command, "ip ", 3) == 0) {
    const char* ip = command + 3;
    if (NetworkManager::setLocalIP(String(ip))) {
      Serial.println(F("IP changed successfully"));
    } else {
      Serial.println(F("Error: Invalid IP"));
    }
    return;
  }
  
  if (strncmp(command, "gateway ", 8) == 0) {
    const char* ip = command + 8;
    if (NetworkManager::setGateway(String(ip))) {
      Serial.println(F("Gateway changed successfully"));
    } else {
      Serial.println(F("Error: Invalid gateway"));
    }
    return;
  }
  
  if (strncmp(command, "subnet ", 7) == 0) {
    const char* ip = command + 7;
    if (NetworkManager::setSubnet(String(ip))) {
      Serial.println(F("Subnet changed successfully"));
    } else {
      Serial.println(F("Error: Invalid subnet"));
    }
    return;
  }
  
  if (strncmp(command, "vmixip ", 7) == 0) {
    const char* ip = command + 7;
    changeVMixIP(ip);
    return;
  }
  
  if (strcmp(command, "reset") == 0) {
    Serial.println(F("Resetting..."));
    delay(100);
    wdt_enable(WDTO_15MS);
    while(1);
  }
  
  if (strcmp(command, "status") == 0) {
    printStatus();
    return;
  }
  
  if (strcmp(command, "ram") == 0) {
    Serial.print(F("Free RAM: "));
    Serial.print(freeRam());
    Serial.println(F(" bytes"));
    return;
  }
  
  if (strcmp(command, "tally") == 0) {
    TallyManager::printCurrentMap();
    return;
  }
  
  if (strcmp(command, "state") == 0) {
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
  Serial.println(F("  gateway X.X.X.X - Change gateway"));
  Serial.println(F("  subnet X.X.X.X  - Change subnet"));
  Serial.println(F("  vmixip X.X.X.X  - Change vMix IP"));
  Serial.println(F("  tally           - Show tally map"));
  Serial.println(F("  state           - Show stored CCU params"));
  Serial.println(F("  ram             - Show free RAM"));
  Serial.println(F("  reset           - Reset Arduino"));
  Serial.println(F("  safemode        - Enter safe mode on next boot"));
  Serial.println(F("  normalboot      - Exit safe mode and restart"));
}

void printStatus() {
  Serial.println();
  Serial.println(F("=== SYSTEM STATUS ==="));
  
  // Safe mode status
  Serial.print(F("Safe Mode: "));
  Serial.println(SafeMode::isActive() ? F("ACTIVE") : F("OFF"));
  Serial.print(F("Reset Count: "));
  Serial.println(SafeMode::getResetCount());
  Serial.print(F("Last Reset: "));
  Serial.println(SafeMode::getResetReasonString());
  Serial.print(F("Uptime: "));
  Serial.print(SafeMode::getUptime());
  Serial.println(F(" seconds"));
  Serial.println();
  
  Serial.print(F("Local IP: "));
  Serial.println(NetworkManager::getLocalIP());
  Serial.print(F("Gateway: "));
  Serial.println(NetworkManager::getGateway());
  Serial.print(F("Subnet: "));
  Serial.println(NetworkManager::getSubnet());
  
  byte vmixip[4];
  VmixConnector::getVmixIP(vmixip);
  Serial.print(F("vMix IP: "));
  Serial.print(vmixip[0]); Serial.print('.');
  Serial.print(vmixip[1]); Serial.print('.');
  Serial.print(vmixip[2]); Serial.print('.');
  Serial.println(vmixip[3]);
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
  
  Serial.print(F("Free RAM: "));
  Serial.print(freeRam());
  Serial.println(F(" bytes"));
  
  Serial.println(F("====================="));
  Serial.println();
}

void loadVMixIPFromEEPROM(byte vmixip[4]) {
  vmixip[0] = StorageManager::readInt(ADDRVMIX0);
  vmixip[1] = StorageManager::readInt(ADDRVMIX1);
  vmixip[2] = StorageManager::readInt(ADDRVMIX2);
  vmixip[3] = StorageManager::readInt(ADDRVMIX3);
  
  // Use defaults if not initialized
  if (vmixip[0] == 0 || vmixip[0] == 255) {
    vmixip[0] = 192;
    vmixip[1] = 168;
    vmixip[2] = 10;
    vmixip[3] = 140;
    
    StorageManager::writeInt(ADDRVMIX0, vmixip[0]);
    StorageManager::writeInt(ADDRVMIX1, vmixip[1]);
    StorageManager::writeInt(ADDRVMIX2, vmixip[2]);
    StorageManager::writeInt(ADDRVMIX3, vmixip[3]);
    
    Serial.println(F("vMix IP initialized to defaults"));
  }
  
  Serial.print(F("vMix IP loaded: "));
  Serial.print(vmixip[0]); Serial.print('.');
  Serial.print(vmixip[1]); Serial.print('.');
  Serial.print(vmixip[2]); Serial.print('.');
  Serial.println(vmixip[3]);
}

void changeVMixIP(const char* ip) {
  int parts[4] = {0, 0, 0, 0};
  int partIdx = 0;
  int start = 0;
  int len = strlen(ip);
  
  for (int i = 0; i <= len && partIdx < 4; i++) {
    if (ip[i] == '.' || ip[i] == '\0') {
      char numBuf[8];
      int numLen = i - start;
      if (numLen > 7) numLen = 7;
      if (numLen > 0) {
        strncpy(numBuf, ip + start, numLen);
        numBuf[numLen] = '\0';
        parts[partIdx++] = atoi(numBuf);
      }
      start = i + 1;
    }
  }
  
  if (partIdx != 4) {
    Serial.println(F("Error: Invalid IP format"));
    return;
  }
  
  for (int i = 0; i < 4; i++) {
    if (parts[i] < 0 || parts[i] > 255) {
      Serial.println(F("Error: IP value out of range"));
      return;
    }
  }
  
  StorageManager::writeInt(ADDRVMIX0, parts[0]);
  StorageManager::writeInt(ADDRVMIX1, parts[1]);
  StorageManager::writeInt(ADDRVMIX2, parts[2]);
  StorageManager::writeInt(ADDRVMIX3, parts[3]);
  
  byte vmixip[4] = {
    (byte)parts[0], 
    (byte)parts[1], 
    (byte)parts[2], 
    (byte)parts[3]
  };
  VmixConnector::setVmixIP(vmixip);
  
  Serial.print(F("vMix IP changed to: "));
  Serial.print(parts[0]); Serial.print('.');
  Serial.print(parts[1]); Serial.print('.');
  Serial.print(parts[2]); Serial.print('.');
  Serial.println(parts[3]);
  Serial.println(F("Saved to EEPROM"));
}

int freeRam() {
  extern int __heap_start, *__brkval;
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}
