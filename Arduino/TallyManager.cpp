/*
 * TallyManager.cpp
 * Tally management implementation
 * Version 3.6
 * 
 * O(n) mapping with n=10 is fast enough and saves 256 bytes of RAM
 */

#include "TallyManager.h"
#include <BMDSDIControl.h>

// Static variable initialization
byte TallyManager::_inputs[MAXTALLIES] = {0};
byte TallyManager::_cams[MAXTALLIES] = {0};
bool TallyManager::_overrideTally = true;

// Cache of last state sent per camera
static bool lastStates[MAX_CAMERAS + 1][2] = {0};
static bool cacheInitialized = false;

// Blackmagic Tally Shield
static BMD_SDITallyControl_I2C sdiTallyControl(BMD_I2C_ADDRESS);

bool TallyManager::begin() {
  loadMap();
  
  bool dummyCCU;
  StorageManager::loadOverrides(_overrideTally, dummyCCU);
  
  sdiTallyControl.begin();
  sdiTallyControl.setOverride(_overrideTally);
  
  cacheInitialized = false;
  
  return true;
}

void TallyManager::loadMap() {
  StorageManager::loadTallyMap(_inputs, _cams);
  printCurrentMap();
}

void TallyManager::saveMap() {
  StorageManager::saveTallyMap(_inputs, _cams);
  Serial.println(F("Tally map saved to EEPROM"));
}

// O(n) mapping - With n=10 it's very fast and saves 256 bytes of RAM
byte TallyManager::mapInputToCamera(byte input) {
  if (input == 0) return 0;
  
  for (int i = 0; i < MAXTALLIES; i++) {
    if (_inputs[i] == input) {
      return _cams[i];
    }
  }
  return 0;
}

void TallyManager::setMapping(byte inputIndex, byte inputNumber, byte cameraId) {
  if (inputIndex < MAXTALLIES) {
    if (inputNumber > 0) _inputs[inputIndex] = inputNumber;
    if (cameraId > 0) _cams[inputIndex] = cameraId;
    saveMap();
  }
}

void TallyManager::setOverride(bool enabled) {
  _overrideTally = enabled;
  sdiTallyControl.setOverride(_overrideTally);
  
  bool currentTally, currentCCU;
  StorageManager::loadOverrides(currentTally, currentCCU);
  StorageManager::saveOverrides(_overrideTally, currentCCU);
  
  Serial.print(F("Tally override: "));
  Serial.println(_overrideTally ? F("ON") : F("OFF"));
  
  // Reset cache to force update if enabled
  if (enabled) {
    cacheInitialized = false;
  }
}

bool TallyManager::getOverride() {
  return _overrideTally;
}

void TallyManager::printCurrentMap() {
  Serial.println(F("Current tally map:"));
  for (int i = 0; i < MAXTALLIES; i++) {
    if (_inputs[i] > 0 || _cams[i] > 0) {
      Serial.print(F("  Idx "));
      Serial.print(i);
      Serial.print(F(": In "));
      Serial.print(_inputs[i]);
      Serial.print(F(" => Cam "));
      Serial.println(_cams[i]);
    }
  }
}

void TallyManager::setTallyState(byte camera, char state) {
  if (camera <= 0 || camera > MAX_CAMERAS) {
    return;
  }
  
  bool program = (state == '1');
  bool preview = (state == '2');
  
  if (_overrideTally) {
    sdiTallyControl.setCameraTally(camera, program, preview);
  }
  
  Serial.print(F("[T] Cam "));
  Serial.print(camera);
  Serial.print(F(": "));
  if (program) Serial.println(F("P"));
  else if (preview) Serial.println(F("V"));
  else Serial.println(F("-"));
}

// Optimized: Only sends actual changes
void TallyManager::setTallyStates(bool states[MAX_CAMERAS + 1][2]) {
  if (!_overrideTally) {
    return;
  }
  
  // First time: send all
  if (!cacheInitialized) {
    Serial.println(F("[T] Init"));
    
    for (int camera = 1; camera <= MAX_CAMERAS; camera++) {
      bool program = states[camera][0];
      bool preview = states[camera][1];
      
      sdiTallyControl.setCameraTally(camera, program, preview);
      
      lastStates[camera][0] = program;
      lastStates[camera][1] = preview;
    }
    
    cacheInitialized = true;
    return;
  }
  
  // Only send cameras that changed
  for (int camera = 1; camera <= MAX_CAMERAS; camera++) {
    bool program = states[camera][0];
    bool preview = states[camera][1];
    
    if (program != lastStates[camera][0] || preview != lastStates[camera][1]) {
      sdiTallyControl.setCameraTally(camera, program, preview);
      
      lastStates[camera][0] = program;
      lastStates[camera][1] = preview;
      
      Serial.print(F("[T] "));
      Serial.print(camera);
      Serial.println(program ? F(":P") : (preview ? F(":V") : F(":")));
    }
  }
}
