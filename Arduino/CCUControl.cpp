/*
 * CCUControl.cpp
 * Camera Control Unit (CCU) implementation
 * Version 3.0
 */

#include "CCUControl.h"
#include "CCUBroadcast.h"
#include "CCUState.h"
#include <BMDSDIControl.h>
#include <avr/wdt.h>

// Static variable initialization
int CCUControl::_activeCameraId = 1;
bool CCUControl::_overrideCameraControl = true;
BMD_SDICameraControl_I2C CCUControl::_sdiCameraControl(BMD_I2C_ADDRESS);

// Parameter cache
CCUControl::ParamCacheEntry CCUControl::_paramCache[CCU_PARAM_CACHE_SIZE] = {{NULL, NULL}};
int CCUControl::_cacheIndex = 0;

bool CCUControl::begin() {
  // Load override state
  bool dummyTally;
  StorageManager::loadOverrides(dummyTally, _overrideCameraControl);
  
  // Initialize CCU shield
  _sdiCameraControl.begin();
  _sdiCameraControl.setOverride(_overrideCameraControl);
  
  // Initialize state storage
  CCUState::begin();
  
  // Initialize cache
  for (int i = 0; i < CCU_PARAM_CACHE_SIZE; i++) {
    _paramCache[i].key = NULL;
    _paramCache[i].def = NULL;
  }
  _cacheIndex = 0;
  
  return true;
}

void CCUControl::setActiveCamera(int cameraId) {
  if (cameraId >= 1 && cameraId <= MAX_CAMERAS) {
    if (_activeCameraId != cameraId) {
      _activeCameraId = cameraId;
      CCUState::setCurrentCamera(cameraId);
      Serial.print(F("Active camera: "));
      Serial.println(_activeCameraId);
    }
  }
}

int CCUControl::getActiveCamera() {
  return _activeCameraId;
}

void CCUControl::setOverride(bool enabled) {
  _overrideCameraControl = enabled;
  _sdiCameraControl.setOverride(_overrideCameraControl);
  
  bool dummyTally;
  StorageManager::loadOverrides(dummyTally, dummyTally);
  StorageManager::saveOverrides(dummyTally, _overrideCameraControl);
  
  Serial.print(F("CCU override: "));
  Serial.println(_overrideCameraControl ? F("ON") : F("OFF"));
}

bool CCUControl::getOverride() {
  return _overrideCameraControl;
}

// Cache lookup - O(1) for frequent parameters
const BMDParamDef* CCUControl::findInCache(const char* key) {
  for (int i = 0; i < CCU_PARAM_CACHE_SIZE; i++) {
    if (_paramCache[i].key != NULL && strcmp(_paramCache[i].key, key) == 0) {
      return _paramCache[i].def;
    }
  }
  return NULL;
}

// Add to cache (circular)
void CCUControl::addToCache(const char* key, const BMDParamDef* def) {
  _paramCache[_cacheIndex].key = key;
  _paramCache[_cacheIndex].def = def;
  _cacheIndex = (_cacheIndex + 1) % CCU_PARAM_CACHE_SIZE;
}

const BMDParamDef* CCUControl::findParamDefByKey(const char *paramKey) {
  // First check cache
  const BMDParamDef* cached = findInCache(paramKey);
  if (cached != NULL) {
    return cached;
  }
  
  // Linear search in parameter table
  for (int i = 0; i < BMD_PARAM_COUNT; i++) {
    if (strcmp(bmdParams[i].paramKey, paramKey) == 0) {
      // Add to cache for future lookups
      addToCache(bmdParams[i].paramKey, &bmdParams[i]);
      return &bmdParams[i];
    }
  }
  
  return NULL;
}

void CCUControl::applySingleValue(const BMDParamDef &def, float valf) {
  if (!_overrideCameraControl) {
    return;
  }

  switch (def.paramType) {
    case TYPE_VOID:
      if ((int)valf == 1) {
        _sdiCameraControl.writeCommandVoid(_activeCameraId, def.groupId, def.paramId);
      }
      break;
      
    case TYPE_BOOLEAN: {
      bool b = (valf == 1.0f);
      _sdiCameraControl.writeCommandBool(_activeCameraId, def.groupId, def.paramId, 0, b);
      break;
    }
    
    case TYPE_INT8: {
      int8_t v = (int8_t)valf;
      _sdiCameraControl.writeCommandInt8(_activeCameraId, def.groupId, def.paramId, 0, v);
      break;
    }
    
    case TYPE_INT16: {
      int16_t v = (int16_t)valf;
      _sdiCameraControl.writeCommandInt16(_activeCameraId, def.groupId, def.paramId, 0, v);
      break;
    }
    
    case TYPE_UINT16: {
      int16_t v = (int16_t)(uint16_t)valf;
      _sdiCameraControl.writeCommandInt16(_activeCameraId, def.groupId, def.paramId, 0, v);
      break;
    }
    
    case TYPE_INT32: {
      int32_t v = (int32_t)valf;
      _sdiCameraControl.writeCommandInt32(_activeCameraId, def.groupId, def.paramId, 0, v);
      break;
    }
    
    case TYPE_INT64: {
      int64_t v = (int64_t)valf;
      _sdiCameraControl.writeCommandInt64(_activeCameraId, def.groupId, def.paramId, 0, v);
      break;
    }
    
    case TYPE_FIXED16: {
      _sdiCameraControl.writeCommandFixed16(_activeCameraId, def.groupId, def.paramId, 0, valf);
      break;
    }
    
    case TYPE_STRING: {
      _sdiCameraControl.writeCommandUTF8(_activeCameraId, def.groupId, def.paramId, 0, "");
      break;
    }
    
    default:
      break;
  }
}

void CCUControl::applyMultiValue(const BMDParamDef &def, float *vals, int count) {
  if (!_overrideCameraControl) {
    return;
  }

  int subCount = min(count, (int)def.subCount);
  if (subCount <= 0 || subCount > BMD_MAX_SUB_COUNT) return;

  switch (def.paramType) {
    case TYPE_INT8: {
      int8_t arr[BMD_MAX_SUB_COUNT];
      for (int i = 0; i < subCount; i++) {
        arr[i] = (int8_t)vals[i];
      }
      _sdiCameraControl.writeCommandInt8(_activeCameraId, def.groupId, def.paramId, 0, arr);
      break;
    }
    
    case TYPE_INT16: {
      int16_t arr[BMD_MAX_SUB_COUNT];
      for (int i = 0; i < subCount; i++) {
        arr[i] = (int16_t)vals[i];
      }
      _sdiCameraControl.writeCommandInt16(_activeCameraId, def.groupId, def.paramId, 0, arr);
      break;
    }
    
    case TYPE_UINT16: {
      int16_t arr[BMD_MAX_SUB_COUNT];
      for (int i = 0; i < subCount; i++) {
        arr[i] = (int16_t)(uint16_t)vals[i];
      }
      _sdiCameraControl.writeCommandInt16(_activeCameraId, def.groupId, def.paramId, 0, arr);
      break;
    }
    
    case TYPE_INT32: {
      int32_t arr[BMD_MAX_SUB_COUNT];
      for (int i = 0; i < subCount; i++) {
        arr[i] = (int32_t)vals[i];
      }
      _sdiCameraControl.writeCommandInt32(_activeCameraId, def.groupId, def.paramId, 0, arr);
      break;
    }
    
    case TYPE_INT64: {
      int64_t arr[BMD_MAX_SUB_COUNT];
      for (int i = 0; i < subCount; i++) {
        arr[i] = (int64_t)vals[i];
      }
      _sdiCameraControl.writeCommandInt64(_activeCameraId, def.groupId, def.paramId, 0, arr);
      break;
    }
    
    case TYPE_FIXED16: {
      float arr[BMD_MAX_SUB_COUNT];
      for (int i = 0; i < subCount; i++) {
        arr[i] = vals[i];
      }
      _sdiCameraControl.writeCommandFixed16(_activeCameraId, def.groupId, def.paramId, 0, arr);
      break;
    }
    
    default:
      break;
  }
}

// Optimized version with const char*
void CCUControl::parseFloatList(const char *str, float *out, int maxCount) {
  if (!str || !out || maxCount <= 0) {
    return;
  }
  
  int idx = 0;
  const char* ptr = str;
  char numBuf[16];
  
  while (*ptr != '\0' && idx < maxCount) {
    // Skip leading spaces
    while (*ptr == ' ') ptr++;
    
    // Find end of number (comma or end of string)
    const char* start = ptr;
    while (*ptr != '\0' && *ptr != ',') ptr++;
    
    // Copy number to temp buffer
    int len = ptr - start;
    if (len > 15) len = 15;
    if (len > 0) {
      strncpy(numBuf, start, len);
      numBuf[len] = '\0';
      
      // Trim trailing spaces
      while (len > 0 && numBuf[len-1] == ' ') {
        numBuf[--len] = '\0';
      }
      
      // Convert to float
      out[idx++] = atof(numBuf);
    }
    
    // Skip comma
    if (*ptr == ',') ptr++;
  }
  
  // Fill remaining with zeros
  for (int i = idx; i < maxCount; i++) {
    out[i] = 0.0f;
  }
}

// String wrapper for compatibility
void CCUControl::parseFloatList(const String &str, float *out, int maxCount) {
  parseFloatList(str.c_str(), out, maxCount);
}

// Main optimized version (const char*)
bool CCUControl::applyParameterByKey(const char *paramKey, const char *value, int cameraId) {
  wdt_reset();
  
  int targetCameraId = (cameraId > 0) ? cameraId : _activeCameraId;
  
  // Quick check: ignore non-CCU parameters
  char first = paramKey[0];
  if (first == 'i' || first == 'p' || first == 's' || first == 'n') {
    if (strcmp(paramKey, "initPreset") == 0 ||
        strcmp(paramKey, "presetParam") == 0 ||
        strcmp(paramKey, "savePresetComplete") == 0 ||
        strcmp(paramKey, "initPresetFragmented") == 0 ||
        strcmp(paramKey, "presetFragment") == 0 ||
        strcmp(paramKey, "savePresetFragmentedComplete") == 0 ||
        strcmp(paramKey, "name") == 0) {
      return true;
    }
  }
  
  // Find parameter definition (uses cache)
  const BMDParamDef *paramDef = findParamDefByKey(paramKey);
  
  if (paramDef == NULL) {
    return false;
  }

  Serial.print(F("[CCU] Cam"));
  Serial.print(targetCameraId);
  Serial.print(F(" "));
  Serial.print(paramKey);
  Serial.print(F("="));
  Serial.println(value);

  int oldCameraId = _activeCameraId;
  _activeCameraId = targetCameraId;

  if (paramDef->hasSubIndices && paramDef->subCount > 1) {
    float values[BMD_MAX_SUB_COUNT];
    parseFloatList(value, values, min((int)paramDef->subCount, BMD_MAX_SUB_COUNT));
    applyMultiValue(*paramDef, values, paramDef->subCount);
  } else {
    float singleValue = atof(value);
    applySingleValue(*paramDef, singleValue);
  }

  _activeCameraId = oldCameraId;
  
  // Store value in state for web sync
  if (targetCameraId == CCUState::getCurrentCamera()) {
    CCUState::storeValue(paramKey, value);
  }
  
  // Broadcast change to connected TCP clients
  CCUBroadcast::sendParamChange(targetCameraId, paramKey, value);
  
  return true;
}

// String wrapper for compatibility
bool CCUControl::applyParameterByKey(const char *paramKey, const String &value, int cameraId) {
  return applyParameterByKey(paramKey, value.c_str(), cameraId);
}
