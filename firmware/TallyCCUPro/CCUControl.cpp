/*
 * CCUControl.cpp
 * Camera Control Unit (CCU) implementation
 * Version 3.8
 */

#include "CCUControl.h"
#include "CCUBroadcast.h"
#include "webserver.h"
#include "SafeMode.h"
#include <BMDSDIControl.h>
#include <avr/wdt.h>
#include <avr/pgmspace.h>
#include <math.h>

// Static variable initialization
int CCUControl::_activeCameraId = 1;
bool CCUControl::_overrideCameraControl = true;
bool CCUControl::_initialized = false;
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
  
  // Initialize cache
  for (int i = 0; i < CCU_PARAM_CACHE_SIZE; i++) {
    _paramCache[i].key = NULL;
    _paramCache[i].def = NULL;
  }
  _cacheIndex = 0;
  
  _initialized = true;
  return true;
}

bool CCUControl::isReady() {
  return _initialized && !SafeMode::isActive();
}

void CCUControl::setActiveCamera(int cameraId) {
  if (cameraId >= 1 && cameraId <= MAX_CAMERAS) {
    if (_activeCameraId != cameraId) {
      _activeCameraId = cameraId;
#if DEBUG_HOT
      Serial.print(F("Active camera: "));
      Serial.println(_activeCameraId);
#endif
    }
  }
}

int CCUControl::getActiveCamera() {
  return _activeCameraId;
}

void CCUControl::setOverride(bool enabled) {
  _overrideCameraControl = enabled;
  // Shield not initialized (Safe Mode): only persist the setting
  if (isReady()) {
    _sdiCameraControl.setOverride(_overrideCameraControl);
  }
  
  bool currentTally, currentCCU;
  StorageManager::loadOverrides(currentTally, currentCCU);
  StorageManager::saveOverrides(currentTally, _overrideCameraControl);
  
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

// Signed 5.11 fixed point: 16.0 would wrap to -16.0
static inline float clampFixed16(float v) {
  return constrain(v, -16.0f, 15.9995f);
}

void CCUControl::applySingleValue(const BMDParamDef &def, float valf) {
  if (!isReady() || !_overrideCameraControl) {
    return;
  }

  switch (def.paramType) {
    case TYPE_VOID:
      if (valf != 0.0f) {
        _sdiCameraControl.writeCommandVoid(_activeCameraId, def.groupId, def.paramId);
      }
      break;
      
    case TYPE_BOOLEAN: {
      bool b = (valf != 0.0f);
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
      _sdiCameraControl.writeCommandFixed16(_activeCameraId, def.groupId, def.paramId, 0, clampFixed16(valf));
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
  if (!isReady() || !_overrideCameraControl) {
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
        arr[i] = clampFixed16(vals[i]);
      }
      _sdiCameraControl.writeCommandFixed16(_activeCameraId, def.groupId, def.paramId, 0, arr);
      break;
    }
    
    default:
      break;
  }
}

// strtod on the original string: keeps exponents (e.g. 2.7e-17)
int CCUControl::parseFloatList(const char *str, float *out, int maxCount) {
  if (!str || !out || maxCount <= 0) {
    return -1;
  }
  
  int idx = 0;
  const char* ptr = str;
  
  for (;;) {
    char* end;
    double v = strtod(ptr, &end);
    if (end == ptr || !isfinite(v)) return -1;   // empty, non-numeric, NaN/Inf
    while (*end == ' ') end++;
    if (*end != ',' && *end != '\0') return -1;  // trailing garbage
    if (idx >= maxCount) return -1;              // too many values
    out[idx++] = (float)v;
    if (*end == '\0') break;
    ptr = end + 1;                               // a token must follow ','
  }
  
  // Fill remaining with zeros
  for (int i = idx; i < maxCount; i++) {
    out[i] = 0.0f;
  }
  return idx;
}

// String wrapper for compatibility
int CCUControl::parseFloatList(const String &str, float *out, int maxCount) {
  return parseFloatList(str.c_str(), out, maxCount);
}

// Case-insensitive PROGMEM word match, trailing spaces allowed
static bool matchWord(const char *s, PGM_P word) {
  size_t n = strlen_P(word);
  if (strncasecmp_P(s, word, n) != 0) return false;
  s += n;
  while (*s == ' ') s++;
  return *s == '\0';
}

int8_t CCUControl::parseBool(const char *str) {
  if (!str) return -1;
  while (*str == ' ') str++;
  if (matchWord(str, PSTR("true")) || matchWord(str, PSTR("on")) || matchWord(str, PSTR("yes"))) return 1;
  if (matchWord(str, PSTR("false")) || matchWord(str, PSTR("off")) || matchWord(str, PSTR("no"))) return 0;
  float v;
  if (parseFloatList(str, &v, 1) != 1) return -1;
  return (v != 0.0f) ? 1 : 0;
}

// Main optimized version (const char*)
// Returns false (and broadcasts nothing) if not ready, key unknown or value malformed
bool CCUControl::applyParameterByKey(const char *paramKey, const char *value, int cameraId, bool fromPreset) {
  wdt_reset();
  
  // No I2C writes without an initialized shield or in Safe Mode
  if (!isReady() || !paramKey || !value) {
    return false;
  }
  
  int targetCameraId = (cameraId > 0) ? cameraId : _activeCameraId;
  if (targetCameraId > MAX_CAMERAS) {
    return false;
  }
  
  // Quick check: ignore non-CCU parameters
  char first = paramKey[0];
  if (first == 'i' || first == 'p' || first == 's' || first == 'n') {
    if (strcmp_P(paramKey, PSTR("initPreset")) == 0 ||
        strcmp_P(paramKey, PSTR("presetParam")) == 0 ||
        strcmp_P(paramKey, PSTR("savePresetComplete")) == 0 ||
        strcmp_P(paramKey, PSTR("initPresetFragmented")) == 0 ||
        strcmp_P(paramKey, PSTR("presetFragment")) == 0 ||
        strcmp_P(paramKey, PSTR("savePresetFragmentedComplete")) == 0 ||
        strcmp_P(paramKey, PSTR("name")) == 0) {
      return true;
    }
  }
  
  // Find parameter definition (uses cache)
  const BMDParamDef *paramDef = findParamDefByKey(paramKey);
  
  if (paramDef == NULL) {
    return false;
  }
  
  // Actions (auto WB, autofocus, reset...) are never replayed from presets
  if (paramDef->paramType == TYPE_VOID && fromPreset) {
    return true;
  }
  
  // Validate before touching the camera
  bool isList = paramDef->hasSubIndices && paramDef->subCount > 1;
  float values[BMD_MAX_SUB_COUNT];
  values[0] = 0.0f;
  int8_t boolVal = -1;
  
  if (isList) {
    if (parseFloatList(value, values, min((int)paramDef->subCount, BMD_MAX_SUB_COUNT)) <= 0) {
      return false;
    }
  } else if (paramDef->paramType == TYPE_BOOLEAN || paramDef->paramType == TYPE_VOID) {
    boolVal = parseBool(value);
    if (boolVal < 0) {
      return false;
    }
    values[0] = boolVal;
  } else if (paramDef->paramType != TYPE_STRING) {
    if (parseFloatList(value, values, 1) != 1) {
      return false;
    }
  }

#if DEBUG_HOT
  Serial.print(F("[CCU] Cam"));
  Serial.print(targetCameraId);
  Serial.print(F(" "));
  Serial.print(paramKey);
  Serial.print(F("="));
  Serial.println(value);
#endif

  int oldCameraId = _activeCameraId;
  _activeCameraId = targetCameraId;

  if (isList) {
    applyMultiValue(*paramDef, values, paramDef->subCount);
  } else {
    applySingleValue(*paramDef, values[0]);
  }

  _activeCameraId = oldCameraId;
  
  // Actions are events, not state: do not echo them (clients would cache them)
  if (paramDef->paramType == TYPE_VOID) {
    return true;
  }
  
  // Booleans are echoed normalized ("true" -> "1")
  char boolBuf[2];
  if (boolVal >= 0) {
    boolBuf[0] = boolVal ? '1' : '0';
    boolBuf[1] = '\0';
    value = boolBuf;
  }
  
  // Broadcast change to connected TCP clients (Companion)
  CCUBroadcast::sendParamChange(targetCameraId, paramKey, value);
  
  // Send SSE event to web client (all cameras, real-time)
  WebServer::sendSSEEvent(targetCameraId, paramKey, value);
  
  return true;
}

// String wrapper for compatibility
bool CCUControl::applyParameterByKey(const char *paramKey, const String &value, int cameraId, bool fromPreset) {
  return applyParameterByKey(paramKey, value.c_str(), cameraId, fromPreset);
}
