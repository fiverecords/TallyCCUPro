/*
 * CCUControl.h
 * Camera Control Unit (CCU) management
 * Version 3.7.1
 */

#ifndef CCU_CONTROL_H
#define CCU_CONTROL_H

#include "Configuration.h"
#include "Storage.h"
#include "bmd_params.h"

// Cache size for frequently used parameters
#define CCU_PARAM_CACHE_SIZE 8

class CCUControl {
  public:
    static bool begin();
    
    // Active camera management
    static void setActiveCamera(int cameraId);
    static int getActiveCamera();
    
    // Override management
    static void setOverride(bool enabled);
    static bool getOverride();
    
    // Apply values to camera parameters
    static void applySingleValue(const BMDParamDef &paramDef, float value);
    static void applyMultiValue(const BMDParamDef &paramDef, float *values, int count);
    
    // Apply parameter by key (optimized const char* version)
    // fromPreset: true when called while applying a stored preset
    static bool applyParameterByKey(const char *paramKey, const char *value, int cameraId = -1, bool fromPreset = false);
    
    // String wrapper for compatibility
    static bool applyParameterByKey(const char *paramKey, const String &value, int cameraId = -1, bool fromPreset = false);
    
    // True when the SDI shield was initialized and we are not in Safe Mode
    static bool isReady();
    
    // Find parameter definition by key (with cache)
    static const BMDParamDef* findParamDefByKey(const char *paramKey);
    
    // Parse comma-separated numbers (strtod, no truncation).
    // Returns values read (rest of out[] zero-filled) or -1 if malformed:
    // empty token, trailing garbage, NaN/Inf or more than maxCount values.
    static int parseFloatList(const char *str, float *out, int maxCount);
    
    // String wrapper for compatibility
    static int parseFloatList(const String &str, float *out, int maxCount);
    
    // 1/true/on/yes -> 1, 0/false/off/no -> 0 (case-insensitive),
    // other numbers -> (v != 0). Returns -1 if not a boolean.
    static int8_t parseBool(const char *str);

  private:
    static int _activeCameraId;
    static bool _overrideCameraControl;
    static bool _initialized;
    static BMD_SDICameraControl_I2C _sdiCameraControl;
    
    // Parameter cache (avoids O(n) search)
    static struct ParamCacheEntry {
      const char* key;
      const BMDParamDef* def;
    } _paramCache[CCU_PARAM_CACHE_SIZE];
    static int _cacheIndex;
    
    static void addToCache(const char* key, const BMDParamDef* def);
    static const BMDParamDef* findInCache(const char* key);
};

#endif // CCU_CONTROL_H
