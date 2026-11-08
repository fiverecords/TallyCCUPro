/*
 * CCUControl.h
 * Camera Control Unit (CCU) management
 */

#ifndef CCU_CONTROL_H
#define CCU_CONTROL_H

#include "Configuration.h"
#include "Storage.h"
#include "bmd_params.h"

// Buffer of canonicalValue(): BMD_MAX_SUB_COUNT x "-2147483648", commas, NUL
#define CCU_VALUE_TEXT_SIZE (BMD_MAX_SUB_COUNT * 12)

class CCUControl {
  public:
    static bool begin();

    // Active camera management
    static void setActiveCamera(int cameraId);
    static int getActiveCamera();

    // CCU override (persisted). OFF: nothing is written to the cameras.
    static void setOverride(bool enabled);
    static bool getOverride();

    // Validates value, writes it to camera cameraId (<= 0: the active
    // camera) and broadcasts the change (TCP 8098 and SSE) in the form of
    // canonicalValue(), i.e. what the camera got.
    // Returns false, writing and broadcasting nothing, if not ready or the
    // camera, key or value is invalid (see validateValue()).
    // fromPreset: true while applying a stored preset (actions and
    // "<key>_0" aliases are then skipped and return true).
    // CCU override OFF (CCU-07): a valid value is neither written nor
    // broadcast, but true is returned (HTTP 200 as in v3.7.1, so the web
    // page shows no error per control and Companion is unaffected): no
    // client is told about a value the camera did not get. Nothing is
    // resent when the override goes back ON.
    static bool applyParameterByKey(const char *paramKey, const char *value, int cameraId = -1, bool fromPreset = false);

    // True if value is valid for def, with the rules of applyParameterByKey:
    // no control characters (bytes < 0x20, 0x7F); numbers as in
    // parseFloatList() (1..subCount values, integers also in 0x hex);
    // booleans as in parseBool(); strings of 1..BMD_STRING_MAX printable
    // ASCII bytes. State from clients (CCUSYNC, syncState) is forwarded
    // through canonicalValue() instead.
    static bool validateValue(const BMDParamDef &def, const char *value);

    // validateValue() plus the text clients are told (CCU-09): integers in
    // decimal, rounded and clamped to the type, with every list element (an
    // empty or missing one as 0); booleans as 1/0; a FIXED16 value with an
    // element out of 5.11 range as clamped (to -16 / 16). Strings and
    // in-range FIXED16 keep the text as sent. Returns value or buf, NULL if
    // invalid.
    static const char* canonicalValue(const BMDParamDef &def, const char *value,
                                      char (&buf)[CCU_VALUE_TEXT_SIZE]);

    // True when the SDI shield was initialized and we are not in Safe Mode
    static bool isReady();

    // Find parameter definition by key ("<key>_0" alias included, see
    // bmdFindParam()). Returns a static RAM copy of the row, valid until the
    // next call; NULL if unknown.
    static const BMDParamDef* findParamDefByKey(const char *paramKey);

    // Parse comma-separated numbers (strtod, no truncation).
    // Returns values read (rest of out[] zero-filled) or -1 if malformed:
    // empty token, trailing garbage, NaN/Inf or more than maxCount values.
    static int parseFloatList(const char *str, float *out, int maxCount);

    // 1/true/on/yes -> 1, 0/false/off/no -> 0 (case-insensitive),
    // other numbers -> (v != 0). Returns -1 if not a boolean.
    static int8_t parseBool(const char *str);

  private:
    static int _activeCameraId;
    static bool _overrideCameraControl;
    static bool _initialized;
    static BMDParamDef _foundDef;  // findParamDefByKey() result
};

#endif // CCU_CONTROL_H
