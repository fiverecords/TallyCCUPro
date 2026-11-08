/*
 * TallyManager.cpp
 * Tally management implementation
 * Version 3.8
 * 
 * O(n) mapping with n=10 is fast enough and saves 256 bytes of RAM
 * Safe Mode support: SDI Shield not initialized when in safe mode
 */

#include "TallyManager.h"
#include "SafeMode.h"
#include <BMDSDIControl.h>

// Static variable initialization
byte TallyManager::_inputs[MAXTALLIES] = {0};
byte TallyManager::_cams[MAXTALLIES] = {0};
bool TallyManager::_overrideTally = true;
bool TallyManager::_reapply = false;

// Cache of last state sent per camera
static bool lastStates[MAX_CAMERAS + 1][2] = {0};
static bool cacheInitialized = false;

// Blackmagic Tally Shield
static BMD_SDITallyControl_I2C sdiTallyControl(BMD_I2C_ADDRESS);

bool TallyManager::begin() {
  loadMap();
  
  bool dummyCCU;
  StorageManager::loadOverrides(_overrideTally, dummyCCU);
  
  // Only initialize SDI shield if NOT in safe mode
  if (!SafeMode::isActive()) {
    sdiTallyControl.begin();
    sdiTallyControl.setOverride(_overrideTally);
  } else {
    Serial.println(F("[Tally] SDI Shield SKIPPED (Safe Mode)"));
  }
  
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
    if (_inputs[i] == input && validSlot(i)) {
      return _cams[i];
    }
  }
  return 0;
}

// Empty (0/0), half-set or out of range (255/255 on a blank EEPROM) slots
// are never evaluated (TAL-07)
bool TallyManager::validSlot(byte index) {
  return index < MAXTALLIES &&
         _inputs[index] >= 1 && _inputs[index] <= MAX_VMIX_INPUTS &&
         _cams[index] >= 1 && _cams[index] <= MAX_CAMERAS;
}

void TallyManager::setMapping(byte inputIndex, byte inputNumber, byte cameraId) {
  if (inputIndex < MAXTALLIES) {
    // 0 (or out of range) leaves that half of the slot unchanged
    if (inputNumber >= 1 && inputNumber <= MAX_VMIX_INPUTS) _inputs[inputIndex] = inputNumber;
    if (cameraId >= 1 && cameraId <= MAX_CAMERAS) _cams[inputIndex] = cameraId;
    // A half-set slot is stored empty: no hidden half that a later request
    // turns on, and getMapping() shows exactly what is stored
    if (!validSlot(inputIndex)) {
      _inputs[inputIndex] = 0;
      _cams[inputIndex] = 0;
    }
    saveMap();
    _reapply = true;
  }
}

void TallyManager::getMapping(byte index, byte &inputNumber, byte &cameraId) {
  if (validSlot(index)) {
    inputNumber = _inputs[index];
    cameraId = _cams[index];
  } else {
    inputNumber = 0;
    cameraId = 0;
  }
}

void TallyManager::setOverride(bool enabled) {
  _overrideTally = enabled;
  
  // Only touch SDI shield if NOT in safe mode
  if (!SafeMode::isActive()) {
    sdiTallyControl.setOverride(_overrideTally);
  }
  
  bool currentTally, currentCCU;
  StorageManager::loadOverrides(currentTally, currentCCU);
  StorageManager::saveOverrides(_overrideTally, currentCCU);
  
  Serial.print(F("Tally override: "));
  Serial.println(_overrideTally ? F("ON") : F("OFF"));
  
  // Full resend of the current vMix state on the next loop pass
  if (enabled) {
    cacheInitialized = false;
    _reapply = true;
  }
}

bool TallyManager::getOverride() {
  return _overrideTally;
}

void TallyManager::printCurrentMap() {
  Serial.println(F("Current tally map:"));
  for (int i = 0; i < MAXTALLIES; i++) {
    if (validSlot(i)) {
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
  
  // Skip SDI output in safe mode
  if (SafeMode::isActive()) {
    return;
  }
  
  bool program = (state == '1');
  bool preview = (state == '2');
  
  if (_overrideTally) {
    sdiTallyControl.setCameraTally(camera, program, preview);
  }
  
  #if DEBUG_HOT
  Serial.print(F("[T] Cam "));
  Serial.print(camera);
  Serial.print(F(": "));
  if (program) Serial.println(F("P"));
  else if (preview) Serial.println(F("V"));
  else Serial.println(F("-"));
  #endif
}

// Optimized: Only sends actual changes
void TallyManager::setTallyStates(bool states[MAX_CAMERAS + 1][2]) {
  if (!_overrideTally) {
    return;
  }
  
  // Skip SDI output in safe mode
  if (SafeMode::isActive()) {
    return;
  }
  
  // First time: send all
  if (!cacheInitialized) {
    #if DEBUG_HOT
    Serial.println(F("[T] Init"));
    #endif
    
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
      
      #if DEBUG_HOT
      Serial.print(F("[T] "));
      Serial.print(camera);
      Serial.println(program ? F(":P") : (preview ? F(":V") : F(":")));
      #endif
    }
  }
}

void TallyManager::clearAll() {
  bool states[MAX_CAMERAS + 1][2] = {{false}};
  setTallyStates(states);
}

void TallyManager::clearMapping(byte index) {
  if (index < MAXTALLIES) {
    _inputs[index] = 0;
    _cams[index] = 0;
    saveMap();
    _reapply = true;
  }
}

bool TallyManager::takeReapply() {
  bool r = _reapply;
  _reapply = false;
  return r;
}
