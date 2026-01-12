/*
 * CCUControl.h
 * Camera Control Unit (CCU) management
 * Version 3.0
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
    static bool applyParameterByKey(const char *paramKey, const char *value, int cameraId = -1);
    
    // String wrapper for compatibility
    static bool applyParameterByKey(const char *paramKey, const String &value, int cameraId = -1);
    
    // Find parameter definition by key (with cache)
    static const BMDParamDef* findParamDefByKey(const char *paramKey);
    
    // Parse float list (optimized const char* version)
    static void parseFloatList(const char *str, float *out, int maxCount);
    
    // String wrapper for compatibility
    static void parseFloatList(const String &str, float *out, int maxCount);

  private:
    static int _activeCameraId;
    static bool _overrideCameraControl;
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
