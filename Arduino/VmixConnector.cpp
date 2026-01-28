/*
 * VmixConnector.cpp
 * vMix connection implementation - Ultra-fast without String
 * Version 3.6
 * 
 * Features tally debounce to avoid flickering
 */

#include "VmixConnector.h"
#include "Network.h"
#include "SdUtils.h"

// Uncomment below to enable debug
//#define DEBUG_VMIX

// Static variable initialization
EthernetClient VmixConnector::_vmixClient;
byte VmixConnector::_vmixip[4] = {192, 168, 10, 140};
bool VmixConnector::_connectEnabled = true;

// Reduced buffers
static char lastValidTallyData[64] = {0};
static char lineBuffer[128];
static char tallyDataBuffer[64];

// Debounce variables
static char pendingTallyData[64] = {0};
static unsigned long pendingTallyTime = 0;
static bool hasPendingTally = false;
const unsigned long TALLY_DEBOUNCE_MS = 50;

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
  
  // Close previous connection if active
  if (_vmixClient.connected()) {
    _vmixClient.stop();
    delay(10);
  }
  
  Serial.println(F("Connecting to vMix..."));
  Serial.print(_vmixip[0]); Serial.print('.');
  Serial.print(_vmixip[1]); Serial.print('.');
  Serial.print(_vmixip[2]); Serial.print('.');
  Serial.print(_vmixip[3]);
  Serial.print(':'); Serial.println(VMIX_PORT);

  IPAddress vmixIPAddress(_vmixip[0], _vmixip[1], _vmixip[2], _vmixip[3]);
  
  if (_vmixClient.connect(vmixIPAddress, VMIX_PORT)) {
    Serial.println(F("vMix connected"));
    _vmixClient.println("SUBSCRIBE TALLY");
    return true;
  } else {
    Serial.println(F("Could not connect to vMix"));
    if (_connectEnabled) {
      Serial.println(F("Auto-reconnection will be attempted"));
    }
    return false;
  }
}

bool VmixConnector::isConnected() {
  return _vmixClient.connected();
}

void VmixConnector::processData() {
  if (!_connectEnabled) {
    if (_vmixClient.connected()) {
      _vmixClient.stop();
    }
    return;
  }
  
  unsigned long currentTime = millis();
  
  bool foundTallyMessage = false;
  int messagesProcessed = 0;
  const int MAX_MESSAGES_PER_CYCLE = 10;
  
  // Process TCP buffer messages
  while (_vmixClient.connected() && _vmixClient.available() && messagesProcessed < MAX_MESSAGES_PER_CYCLE) {
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
    
    // Check if TALLY message
    if (len > 9 && strncmp(lineBuffer, "TALLY OK ", 9) == 0) {
      int dataLen = len - 9;
      if (dataLen > 63) dataLen = 63;
      strncpy(tallyDataBuffer, lineBuffer + 9, dataLen);
      tallyDataBuffer[dataLen] = '\0';
      foundTallyMessage = true;
    }
  }
  
  // DEBOUNCE: Save pending state
  if (foundTallyMessage && tallyDataBuffer[0] != '\0') {
    if (strcmp(tallyDataBuffer, lastValidTallyData) != 0) {
      strncpy(pendingTallyData, tallyDataBuffer, 63);
      pendingTallyData[63] = '\0';
      pendingTallyTime = currentTime;
      hasPendingTally = true;
    }
  }
  
  // Apply pending state after debounce
  if (hasPendingTally && (currentTime - pendingTallyTime >= TALLY_DEBOUNCE_MS)) {
    if (strcmp(pendingTallyData, lastValidTallyData) != 0) {
      Serial.print(F("[VMIX] "));
      Serial.println(pendingTallyData);
      
      if (!g_isBusyWithSD) {
        applyTallyDirectly(pendingTallyData);
        strncpy(lastValidTallyData, pendingTallyData, 63);
        lastValidTallyData[63] = '\0';
      }
    }
    hasPendingTally = false;
  }
  
  // Reconnection state machine
  static unsigned long lastReconnectCheck = 0;
  static unsigned long lastReconnectAttempt = 0;
  static int reconnectFailCount = 0;
  
  if (!_vmixClient.connected() && _connectEnabled) {
    if (currentTime - lastReconnectCheck > 500) {
      lastReconnectCheck = currentTime;
      
      if (currentTime - lastReconnectAttempt > VMIX_RECONNECT_DELAY) {
        Serial.println(F("Attempting vMix reconnection..."));
        
        IPAddress vmixIPAddress(_vmixip[0], _vmixip[1], _vmixip[2], _vmixip[3]);
        
        if (_vmixClient.connect(vmixIPAddress, VMIX_PORT)) {
          Serial.println(F("vMix reconnected"));
          _vmixClient.println("SUBSCRIBE TALLY");
          reconnectFailCount = 0;
          lastValidTallyData[0] = '\0';
          hasPendingTally = false;
        } else {
          Serial.println(F("Failed to reconnect to vMix"));
          reconnectFailCount++;
          
          if (reconnectFailCount > 5) {
            lastReconnectAttempt = currentTime + (reconnectFailCount * 5000);
          }
        }
        
        lastReconnectAttempt = currentTime;
      }
    }
  }
}

bool VmixConnector::setVmixIP(const byte vmixip[4]) {
  memcpy(_vmixip, vmixip, 4);
  
  const int ADDRVMIX0 = 20;
  const int ADDRVMIX1 = 22;
  const int ADDRVMIX2 = 24;
  const int ADDRVMIX3 = 26;
  
  StorageManager::writeInt(ADDRVMIX0, vmixip[0]);
  StorageManager::writeInt(ADDRVMIX1, vmixip[1]);
  StorageManager::writeInt(ADDRVMIX2, vmixip[2]);
  StorageManager::writeInt(ADDRVMIX3, vmixip[3]);
  
  Serial.println(F("vMix IP saved permanently to EEPROM"));
  Serial.print(vmixip[0]); Serial.print('.');
  Serial.print(vmixip[1]); Serial.print('.');
  Serial.print(vmixip[2]); Serial.print('.');
  Serial.println(vmixip[3]);
  
  lastValidTallyData[0] = '\0';
  hasPendingTally = false;
  
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
  
  if (!enabled && _vmixClient.connected()) {
    _vmixClient.stop();
    Serial.println(F("vMix connection closed by user"));
  } else if (enabled && !_vmixClient.connected()) {
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
  
  bool states[MAX_CAMERAS + 1][2] = {0};
  bool stateChanged = false;
  
  int length = strlen(tallyData);
  if (length > MAXTALLIES) length = MAXTALLIES;
  
  for (int i = 0; i < length; i++) {
    char state = tallyData[i];
    
    // Quick skip for '0' states
    if (state == '0') continue;
    
    byte cameraId = TallyManager::mapInputToCamera(i + 1);
    
    if (cameraId > 0 && cameraId <= MAX_CAMERAS) {
      states[cameraId][0] = (state == '1'); // Program
      states[cameraId][1] = (state == '2'); // Preview
      stateChanged = true;
    }
  }
  
  if (stateChanged) {
    TallyManager::setTallyStates(states);
  }
}
