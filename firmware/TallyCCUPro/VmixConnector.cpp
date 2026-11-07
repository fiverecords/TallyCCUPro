/*
 * VmixConnector.cpp
 * vMix connection implementation - Ultra-fast without String
 * Version 3.8
 * 
 * Features tally debounce to avoid flickering
 */

#include "VmixConnector.h"
#include "Network.h"
#include "SdUtils.h"

// Uncomment below to enable debug
//#define DEBUG_VMIX

// W5x00 socket status register values (Sn_SR)
#define VMIX_SNSR_ESTABLISHED 0x17
#define VMIX_SNSR_CLOSE_WAIT  0x1C

// Static variable initialization
EthernetClient VmixConnector::_vmixClient;
byte VmixConnector::_vmixip[4] = {192, 168, 10, 140};
bool VmixConnector::_connectEnabled = true;

// Reduced buffers
static char lastValidTallyData[MAX_VMIX_INPUTS + 1] = {0};
static char lineBuffer[128];
static char tallyDataBuffer[MAX_VMIX_INPUTS + 1];

// Debounce variables
static char pendingTallyData[MAX_VMIX_INPUTS + 1] = {0};
static unsigned long pendingTallyTime = 0;
static bool hasPendingTally = false;
const unsigned long TALLY_DEBOUNCE_MS = 50;

static const char SUBSCRIBE_CMD[] PROGMEM = "SUBSCRIBE TALLY\r\n";

// Turn every tally off and forget the last vMix state
static void clearTally() {
  TallyManager::clearAll();
  lastValidTallyData[0] = '\0';
  hasPendingTally = false;
}

bool VmixConnector::begin() {
  bool dummyTally, dummyCCU;
  StorageManager::loadOverrides(dummyTally, dummyCCU, _connectEnabled);
  
  Serial.print(F("vMix connection state loaded: "));
  Serial.println(_connectEnabled ? F("ON") : F("OFF"));
  
  if (_connectEnabled) {
    return connect();
  } else {
    Serial.println(F("vMix connection disabled by user"));
    return false;
  }
}

bool VmixConnector::begin(const byte vmixip[4]) {
  memcpy(_vmixip, vmixip, 4);
  
  bool dummyTally, dummyCCU;
  StorageManager::loadOverrides(dummyTally, dummyCCU, _connectEnabled);
  
  Serial.print(F("vMix connection state loaded: "));
  Serial.println(_connectEnabled ? F("ON") : F("OFF"));
  
  if (_connectEnabled) {
    return connect();
  } else {
    Serial.println(F("vMix connection disabled by user"));
    return false;
  }
}

bool VmixConnector::connect() {
  if (!_connectEnabled) {
    Serial.println(F("vMix connection disabled by user"));
    return false;
  }
  
  Serial.println(F("Connecting to vMix..."));
  Serial.print(_vmixip[0]); Serial.print('.');
  Serial.print(_vmixip[1]); Serial.print('.');
  Serial.print(_vmixip[2]); Serial.print('.');
  Serial.print(_vmixip[3]);
  Serial.print(':'); Serial.println(VMIX_PORT);
  
  if (openSocket()) {
    Serial.println(F("vMix connected"));
    return true;
  } else {
    Serial.println(F("Could not connect to vMix"));
    if (_connectEnabled) {
      Serial.println(F("Auto-reconnection will be attempted"));
    }
    return false;
  }
}

// Closes any previous connection, then connects with a short timeout.
// A refused connect() returns with _sockindex still set on a socket that is
// already free, so it is released here before another module can take it.
bool VmixConnector::openSocket() {
  closeSocket();
  
  IPAddress vmixIPAddress(_vmixip[0], _vmixip[1], _vmixip[2], _vmixip[3]);
  _vmixClient.setConnectionTimeout(VMIX_CONNECT_TIMEOUT_MS);
  
  if (!_vmixClient.connect(vmixIPAddress, VMIX_PORT)) {
    _vmixClient.stop();
    return false;
  }

  char cmd[sizeof(SUBSCRIBE_CMD)];
  memcpy_P(cmd, SUBSCRIBE_CMD, sizeof(cmd));
  _vmixClient.write((const uint8_t*)cmd, sizeof(cmd) - 1);
  
  // Apply the initial state vMix sends after SUBSCRIBE
  lastValidTallyData[0] = '\0';
  hasPendingTally = false;
  return true;
}

// True only while _vmixClient holds our own connection to vMix. A socket that
// is closed or was reused by HTTP/Companion is forgotten WITHOUT stop(), which
// would close a connection that belongs to someone else.
bool VmixConnector::ownsSocket() {
  if (_vmixClient.getSocketNumber() >= MAX_SOCK_NUM) return false;
  
  uint8_t s = _vmixClient.status();
  if ((s == VMIX_SNSR_ESTABLISHED || s == VMIX_SNSR_CLOSE_WAIT) &&
      _vmixClient.remotePort() == VMIX_PORT &&
      _vmixClient.remoteIP() == _vmixip) {
    return true;
  }
  
  dropSocket();
  return false;
}

// Forget the socket index without touching the W5500
void VmixConnector::dropSocket() {
  _vmixClient = EthernetClient();
}

// Close our own connection (FIN), or just forget a socket that is not ours
void VmixConnector::closeSocket() {
  if (ownsSocket()) {
    _vmixClient.stop();
  }
}

bool VmixConnector::isConnected() {
  return ownsSocket() && _vmixClient.connected();
}

void VmixConnector::processData() {
  static bool wasConnected = false;
  static bool disabledCleared = false;
  
  if (!_connectEnabled) {
    closeSocket();
    wasConnected = false;
    // Turn tally off once when entering the disabled state
    if (!disabledCleared) {
      clearTally();
      disabledCleared = true;
    }
    return;
  }
  disabledCleared = false;
  
  unsigned long currentTime = millis();
  
  // Also drops a socket that is no longer ours before reading from it
  bool connectedNow = isConnected();
  
  // Connected -> disconnected: do not leave the last tally frozen
  if (wasConnected && !connectedNow) {
    Serial.println(F("vMix connection lost"));
    closeSocket();
    clearTally();
  }
  wasConnected = connectedNow;
  
  bool foundTallyMessage = false;
  int messagesProcessed = 0;
  const int MAX_MESSAGES_PER_CYCLE = 10;
  
  // Process TCP buffer messages
  while (connectedNow && _vmixClient.available() && messagesProcessed < MAX_MESSAGES_PER_CYCLE) {
    int len = 0;
    char ch;
    
    // Read until \r or \n
    while (_vmixClient.available() && len < 127) {
      ch = _vmixClient.read();
      if (ch == '\r' || ch == '\n') break;
      lineBuffer[len++] = ch;
    }
    lineBuffer[len] = '\0';
    
    messagesProcessed++;
    
    if (len == 0) {
      continue;
    }
    
    #ifdef DEBUG_VMIX
    Serial.print(F("[vmixdata] Received: "));
    Serial.println(lineBuffer);
    #endif
    
    // Check if TALLY message. A line longer than lineBuffer is cut: the first
    // chunk keeps the prefix and inputs 1..MAX_VMIX_INPUTS, and the tail is
    // read as a fragment without prefix and ignored.
    if (len > 9 && strncmp_P(lineBuffer, PSTR("TALLY OK "), 9) == 0) {
      int dataLen = len - 9;
      if (dataLen > MAX_VMIX_INPUTS) dataLen = MAX_VMIX_INPUTS;
      memcpy(tallyDataBuffer, lineBuffer + 9, dataLen);
      tallyDataBuffer[dataLen] = '\0';
      foundTallyMessage = true;
    }
  }
  
  // DEBOUNCE: always follow the latest data received
  if (foundTallyMessage && tallyDataBuffer[0] != '\0') {
    if (strcmp(tallyDataBuffer, lastValidTallyData) == 0) {
      hasPendingTally = false; // Back to the applied state
    } else if (!hasPendingTally || strcmp(tallyDataBuffer, pendingTallyData) != 0) {
      strcpy(pendingTallyData, tallyDataBuffer);
      pendingTallyTime = currentTime;
      hasPendingTally = true;
    }
  }
  
  // Apply pending state after debounce (kept pending while the SD is busy)
  if (hasPendingTally && !g_isBusyWithSD &&
      (currentTime - pendingTallyTime >= TALLY_DEBOUNCE_MS)) {
    applyTallyDirectly(pendingTallyData);
    strcpy(lastValidTallyData, pendingTallyData);
    hasPendingTally = false;
    
    #if DEBUG_HOT
    Serial.print(F("[VMIX] "));
    Serial.println(lastValidTallyData);
    #endif
  }
  
  // Reconnection state machine
  static unsigned long lastReconnectCheck = 0;
  static unsigned long lastReconnectAttempt = 0;
  static int reconnectFailCount = 0;
  
  if (!connectedNow) {
    if (currentTime - lastReconnectCheck > 500) {
      lastReconnectCheck = currentTime;
      
      // Progressive backoff: increase delay after repeated failures
      unsigned long reconnectDelay = VMIX_RECONNECT_DELAY;
      if (reconnectFailCount > 5) {
        reconnectDelay += (unsigned long)(reconnectFailCount - 5) * 5000UL;
        if (reconnectDelay > 120000UL) reconnectDelay = 120000UL; // Cap at 2 min
      }
      
      if (currentTime - lastReconnectAttempt > reconnectDelay) {
        Serial.println(F("Attempting vMix reconnection..."));
        
        if (openSocket()) {
          Serial.println(F("vMix reconnected"));
          reconnectFailCount = 0;
        } else {
          Serial.println(F("Failed to reconnect to vMix"));
          reconnectFailCount++;
          lastReconnectAttempt = currentTime;
        }
      }
    }
  }
}

bool VmixConnector::setVmixIP(const byte vmixip[4]) {
  // Close the old connection while the old IP still identifies it as ours
  closeSocket();
  
  memcpy(_vmixip, vmixip, 4);
  
  StorageManager::writeInt(EEPROM_VMIX_IP0, vmixip[0]);
  StorageManager::writeInt(EEPROM_VMIX_IP1, vmixip[1]);
  StorageManager::writeInt(EEPROM_VMIX_IP2, vmixip[2]);
  StorageManager::writeInt(EEPROM_VMIX_IP3, vmixip[3]);
  
  Serial.println(F("vMix IP saved permanently to EEPROM"));
  Serial.print(vmixip[0]); Serial.print('.');
  Serial.print(vmixip[1]); Serial.print('.');
  Serial.print(vmixip[2]); Serial.print('.');
  Serial.println(vmixip[3]);
  
  // The old vMix state no longer applies
  clearTally();
  
  return _connectEnabled ? connect() : false;
}

void VmixConnector::getVmixIP(byte vmixip[4]) {
  memcpy(vmixip, _vmixip, 4);
}

void VmixConnector::setConnectEnabled(bool enabled) {
  _connectEnabled = enabled;
  
  bool currentTally, currentCCU, currentVmix;
  StorageManager::loadOverrides(currentTally, currentCCU, currentVmix);
  StorageManager::saveOverrides(currentTally, currentCCU, _connectEnabled);
  
  Serial.print(F("vMix connection state changed to: "));
  Serial.println(_connectEnabled ? F("ON") : F("OFF"));
  
  if (!enabled) {
    bool wasUp = isConnected();
    closeSocket();
    if (wasUp) {
      Serial.println(F("vMix connection closed by user"));
    }
    clearTally();
  } else if (!isConnected()) {
    Serial.println(F("vMix connection enabled, attempting to connect..."));
    connect();
  }
}

bool VmixConnector::getConnectEnabled() {
  return _connectEnabled;
}

void VmixConnector::applyTallyDirectly(const char* tallyData) {
  #ifdef DEBUG_VMIX
  Serial.print(F("[VMIX] Data: "));
  Serial.println(tallyData);
  #endif
  
  bool states[MAX_CAMERAS + 1][2] = {{false}};
  
  // Walk the map slots, not the string: any input 1..MAX_VMIX_INPUTS works and
  // several inputs on one camera are OR-ed instead of overwriting each other
  byte length = (byte)strnlen(tallyData, MAX_VMIX_INPUTS);
  
  for (byte slot = 0; slot < MAXTALLIES; slot++) {
    byte input, cameraId;
    TallyManager::getMapping(slot, input, cameraId);
    
    if (input < 1 || input > length || cameraId < 1 || cameraId > MAX_CAMERAS) continue;
    
    char state = tallyData[input - 1];
    if (state == '1') states[cameraId][0] = true;       // Program
    else if (state == '2') states[cameraId][1] = true;  // Preview
  }
  
  // Program wins over preview
  for (byte camera = 1; camera <= MAX_CAMERAS; camera++) {
    if (states[camera][0]) states[camera][1] = false;
  }
  
  // Always apply - setTallyStates has its own internal cache
  // that only sends I2C commands for cameras that actually changed
  TallyManager::setTallyStates(states);
}
