/*
 * CCUControl.cpp
 * Camera Control Unit (CCU) implementation
 */

#include "CCUControl.h"
#include "CCUBroadcast.h"
#include "webserver.h"
#include "SafeMode.h"
#include <BMDSDIControl.h>
#include <avr/wdt.h>
#include <avr/pgmspace.h>
#include <math.h>
#include <limits.h>

// Static variable initialization
int CCUControl::_activeCameraId = 1;
bool CCUControl::_overrideCameraControl = true;
bool CCUControl::_initialized = false;

BMDParamDef CCUControl::_foundDef;

namespace {

BMD_SDICameraControl_I2C sdi(BMD_I2C_ADDRESS);

// A value parsed and range-checked for its parameter type
struct Parsed {
  union {
    float f[BMD_MAX_SUB_COUNT];      // TYPE_FIXED16, clamped to 5.11
    long i[BMD_MAX_SUB_COUNT];       // integer types, clamped to the type
  };
  int8_t b;                          // TYPE_BOOLEAN / TYPE_VOID: 0 or 1
  uint8_t clamped;                   // TYPE_FIXED16: bit k set if f[k] was clamped
};

static_assert(BMD_MAX_SUB_COUNT <= 8, "Parsed::clamped: one bit per element");
static_assert(CCU_VALUE_TEXT_SIZE >= BMD_MAX_SUB_COUNT * 12,
              "CCU_VALUE_TEXT_SIZE: an int32 list (\"-2147483648,\" per element)");

// Bytes < 0x20 and 0x7F would break the CCU line (TCP 8098) and the SSE
// event built from the value (TCP-07)
bool hasControlChars(const char *s) {
  for (; *s; s++) {
    uint8_t c = (uint8_t)*s;
    if (c < 0x20 || c == 0x7F) return true;
  }
  return false;
}

// Signed 5.11 fixed point: 16.0 would wrap to -16.0
inline float clampFixed16(float v) {
  return constrain(v, -16.0f, 15.9995f);
}

// Range of an integer type (no TYPE_INT64 rows: see bmd_params.cpp)
void intRange(ParamType t, long &lo, long &hi) {
  switch (t) {
    case TYPE_INT8:   lo = -128;     hi = 127;      break;
    case TYPE_INT16:  lo = -32768;   hi = 32767;    break;
    case TYPE_UINT16: lo = 0;        hi = 65535;    break;
    default:          lo = -2147483647L - 1; hi = 2147483647L; break;   // INT32
  }
}

// Integer field at s: an exact integer (optional sign, decimal or 0x hex,
// so a BCD date does not go through a 24-bit float), else any strtod
// number rounded to nearest. Saturated to the long range. *end is set past
// the field (the caller checks what follows).
bool parseIntField(const char *s, const char **end, long *out) {
  const char *p = (*s == '-' || *s == '+') ? s + 1 : s;
  bool hex = (p[0] == '0' && (p[1] | 0x20) == 'x');
  char *e;
  long v = strtol(s, &e, hex ? 16 : 10);   // saturates; base 10: no octal
  if (e > p + (hex ? 2 : 0) && (*e == '\0' || *e == ',' || *e == ' ')) {
    *out = v;
    *end = e;
    return true;
  }
  if (hex) return false;
  double d = strtod(s, &e);
  if (e == s || !isfinite(d)) return false;
  if (d <= (double)LONG_MIN) *out = LONG_MIN;
  else if (d >= (double)LONG_MAX) *out = LONG_MAX;
  else *out = lround(d);
  *end = e;
  return true;
}

// Comma-separated numbers, positional: inside a list an empty field is 0
// (v3.7.1 also sent 0, e.g. Companion's default "," for
// exposure_and_focus_tools). A non-numeric field, NaN/Inf, trailing
// garbage or more than maxCount fields reject the whole list. Floats go to
// ff (strtod), integers (ff == NULL) to fi through parseIntField().
// Returns fields read (the rest zero-filled) or -1.
int parseList(const char *str, int maxCount, float *ff, long *fi) {
  bool isList = (strchr(str, ',') != NULL);
  int idx = 0;
  const char *ptr = str;

  for (;;) {
    while (*ptr == ' ') ptr++;
    if (idx >= maxCount) return -1;              // too many values
    const char *end = ptr;
    if (*ptr == ',' || *ptr == '\0') {
      if (!isList) return -1;                    // a single value can't be empty
      if (ff) ff[idx] = 0.0f; else fi[idx] = 0;
    } else if (ff) {
      char *e;
      double v = strtod(ptr, &e);
      if (e == ptr || !isfinite(v)) return -1;   // non-numeric, NaN/Inf
      // No hex floats: avr-libc's strtod stops at the 'x', a host's does not
      if (memchr(ptr, 'x', e - ptr) || memchr(ptr, 'X', e - ptr)) return -1;
      ff[idx] = (float)v;
      end = e;
    } else if (!parseIntField(ptr, &end, &fi[idx])) {
      return -1;
    }
    idx++;
    while (*end == ' ') end++;
    if (*end == '\0') break;
    if (*end != ',') return -1;                  // trailing garbage
    ptr = end + 1;
  }

  for (int i = idx; i < maxCount; i++) {
    if (ff) ff[i] = 0.0f; else fi[i] = 0;
  }
  return idx;
}

// Parses value for def into p (see CCUControl::validateValue())
bool parseValue(const BMDParamDef &def, const char *value, Parsed &p) {
  if (!value || hasControlChars(value)) return false;
  // 1..BMD_MAX_SUB_COUNT, checked at compile time in bmd_params.cpp
  uint8_t n = def.subCount;

  switch (def.paramType) {
    case TYPE_VOID:
    case TYPE_BOOLEAN:
      p.b = CCUControl::parseBool(value);
      return p.b >= 0;

    case TYPE_STRING: {
      size_t len = strlen(value);
      if (len == 0 || len > BMD_STRING_MAX) return false;
      for (const char *s = value; *s; s++) {
        if ((uint8_t)*s > 0x7E) return false;
      }
      return true;
    }

    case TYPE_FIXED16:
      if (parseList(value, n, p.f, NULL) < 1) return false;
      p.clamped = 0;
      for (uint8_t k = 0; k < n; k++) {
        float v = clampFixed16(p.f[k]);
        if (v != p.f[k]) p.clamped |= (uint8_t)(1 << k);
        p.f[k] = v;
      }
      return true;

    case TYPE_INT8:
    case TYPE_INT16:
    case TYPE_UINT16:
    case TYPE_INT32: {
      if (parseList(value, n, NULL, p.i) < 1) return false;
      long lo, hi;
      intRange(def.paramType, lo, hi);
      for (uint8_t k = 0; k < n; k++) p.i[k] = constrain(p.i[k], lo, hi);
      return true;
    }

    default:
      return false;
  }
}

// Element of a rebuilt FIXED16 list: a clamped one as the protocol limit
// (-16, 16), else at most 4 decimals (the 5.11 step is 1/2048), no
// trailing zeros. At most 8 chars ("-15.9995"). Returns the end (NUL).
char* fixedText(float v, bool clamped, char *s) {
  uint32_t t = clamped ? 160000UL : (uint32_t)lround(fabsf(v) * 10000.0f);
  if (v < 0 && t) *s++ = '-';
  ultoa(t / 10000, s, 10);
  s += strlen(s);
  uint16_t frac = (uint16_t)(t % 10000);
  if (frac) {
    *s++ = '.';
    for (uint16_t div = 1000; frac; div /= 10) {
      *s++ = (char)('0' + frac / div);
      frac %= div;
    }
  }
  *s = '\0';
  return s;
}

// Text that clients are told (TCP 8098, SSE) for a parsed value: what the
// camera got (CCU-09). Integers in decimal, rounded and clamped, every list
// element (an empty or missing one as the 0 sent); booleans as 1/0; a
// FIXED16 value with a clamped element rebuilt (see fixedText()). Other
// values (strings, in-range FIXED16) keep the text as sent: returns value.
// Else returns buf (CCU_VALUE_TEXT_SIZE bytes).
const char* valueText(const BMDParamDef &d, const Parsed &p, const char *value, char *buf) {
  char *s = buf;
  switch (d.paramType) {
    case TYPE_VOID:
    case TYPE_BOOLEAN:
      buf[0] = p.b ? '1' : '0';
      buf[1] = '\0';
      return buf;

    case TYPE_FIXED16:
      if (!p.clamped) return value;
      for (uint8_t k = 0; k < d.subCount; k++) {
        if (k) *s++ = ',';
        s = fixedText(p.f[k], p.clamped & (1 << k), s);
      }
      return buf;

    case TYPE_INT8:
    case TYPE_INT16:
    case TYPE_UINT16:
    case TYPE_INT32:
      for (uint8_t k = 0; k < d.subCount; k++) {
        if (k) *s++ = ',';
        ltoa(p.i[k], s, 10);
        s += strlen(s);
      }
      return buf;

    default:
      return value;
  }
}

// Array writes of the library are templates on the array length: all N
// elements go on the wire, so lists use arrays of exactly subCount (CCU-08).
// The library takes the arrays by non-const reference (T (&)[N]): a const
// array does not compile with the official Blackmagic library.
template <unsigned N> inline void sdiWrite(uint8_t c, const BMDParamDef &d, int8_t (&a)[N]) {
  sdi.writeCommandInt8(c, d.groupId, d.paramId, 0, a);
}
template <unsigned N> inline void sdiWrite(uint8_t c, const BMDParamDef &d, int16_t (&a)[N]) {
  sdi.writeCommandInt16(c, d.groupId, d.paramId, 0, a);
}
template <unsigned N> inline void sdiWrite(uint8_t c, const BMDParamDef &d, int32_t (&a)[N]) {
  sdi.writeCommandInt32(c, d.groupId, d.paramId, 0, a);
}
template <unsigned N> inline void sdiWrite(uint8_t c, const BMDParamDef &d, float (&a)[N]) {
  sdi.writeCommandFixed16(c, d.groupId, d.paramId, 0, a);
}

// Values are already in range: the int16_t cast keeps UINT16's bit pattern
template <typename T, unsigned N, typename S>
void writeArray(uint8_t c, const BMDParamDef &d, const S *v) {
  T a[N];
  for (uint8_t k = 0; k < N; k++) a[k] = (T)v[k];
  sdiWrite(c, d, a);
}

template <typename T, typename S>
bool writeList(uint8_t c, const BMDParamDef &d, const S *v) {
  static_assert(BMD_MAX_SUB_COUNT == 5, "writeList(): one case per list size");
  switch (d.subCount) {
    case 2: writeArray<T, 2>(c, d, v); return true;
    case 3: writeArray<T, 3>(c, d, v); return true;
    case 4: writeArray<T, 4>(c, d, v); return true;
    case 5: writeArray<T, 5>(c, d, v); return true;
  }
  return false;
}

// Writes a parsed value to camera cam. Always operation 0 (assign): the
// protocol is write-only, so offsets would lose the absolute value that the
// web page and Companion cache (CCU-12, protocol unchanged).
bool writeValue(uint8_t cam, const BMDParamDef &d, const Parsed &p, const char *str) {
  bool list = d.subCount > 1;
  switch (d.paramType) {
    case TYPE_VOID:
      if (p.b) sdi.writeCommandVoid(cam, d.groupId, d.paramId);
      return true;

    case TYPE_BOOLEAN:
      sdi.writeCommandBool(cam, d.groupId, d.paramId, 0, p.b != 0);
      return true;

    case TYPE_INT8:
      if (list) return writeList<int8_t>(cam, d, p.i);
      sdi.writeCommandInt8(cam, d.groupId, d.paramId, 0, (int8_t)p.i[0]);
      return true;

    case TYPE_INT16:
    case TYPE_UINT16:
      if (list) return writeList<int16_t>(cam, d, p.i);
      sdi.writeCommandInt16(cam, d.groupId, d.paramId, 0, (int16_t)p.i[0]);
      return true;

    case TYPE_INT32:
      if (list) return writeList<int32_t>(cam, d, p.i);
      sdi.writeCommandInt32(cam, d.groupId, d.paramId, 0, (int32_t)p.i[0]);
      return true;

    case TYPE_FIXED16:
      if (list) return writeList<float>(cam, d, p.f);
      sdi.writeCommandFixed16(cam, d.groupId, d.paramId, 0, p.f[0]);
      return true;

    case TYPE_STRING:
      sdi.writeCommandUTF8(cam, d.groupId, d.paramId, 0, str);
      return true;

    default:
      return false;
  }
}

}  // namespace

bool CCUControl::begin() {
  // Load override state
  bool dummyTally;
  StorageManager::loadOverrides(dummyTally, _overrideCameraControl);

  // Initialize CCU shield
  sdi.begin();
  sdi.setOverride(_overrideCameraControl);

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
    sdi.setOverride(_overrideCameraControl);
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

const BMDParamDef* CCUControl::findParamDefByKey(const char *paramKey) {
  return bmdFindParam(paramKey, &_foundDef) >= 0 ? &_foundDef : NULL;
}

// strtod on the original string: keeps exponents (e.g. 2.7e-17)
int CCUControl::parseFloatList(const char *str, float *out, int maxCount) {
  if (!str || !out || maxCount <= 0) {
    return -1;
  }
  return parseList(str, maxCount, out, NULL);
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

bool CCUControl::validateValue(const BMDParamDef &def, const char *value) {
  Parsed p;
  return parseValue(def, value, p);
}

const char* CCUControl::canonicalValue(const BMDParamDef &def, const char *value,
                                       char (&buf)[CCU_VALUE_TEXT_SIZE]) {
  Parsed p;
  if (!parseValue(def, value, p)) return NULL;
  return valueText(def, p, value, buf);
}

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

  // Local copy of the flash table row
  BMDParamDef def;
  int8_t keyLen = bmdFindParam(paramKey, &def);
  if (keyLen < 0) {
    return false;
  }

  // The "_0" alias is for live commands only. A preset skips it as v3.7.1
  // did: Companion stores a stale timecode_source_0, and the preset skip
  // list (SdUtils) only knows canonical keys.
  bool alias = paramKey[keyLen] != '\0';
  if (alias && fromPreset) {
    return true;
  }

  // Actions (auto WB, autofocus, reset...) are never replayed from presets
  if (def.paramType == TYPE_VOID && fromPreset) {
    return true;
  }

  // Validate before touching the camera
  Parsed parsed;
  if (!parseValue(def, value, parsed)) {
    return false;
  }

#if DEBUG_HOT
  Serial.print(F("[CCU] Cam"));
  Serial.print(targetCameraId);
  Serial.print(F(" "));
  Serial.print(paramKey);
  Serial.print(F("="));
  Serial.println(value);
#endif

  // Override OFF: the camera gets nothing, so no client is told it did
  if (!_overrideCameraControl) {
    return true;
  }

  if (!writeValue((uint8_t)targetCameraId, def, parsed, value)) {
    return false;
  }

  // Actions are events, not state: do not echo them (clients would cache them)
  if (def.paramType == TYPE_VOID) {
    return true;
  }

  // Echo what the camera got ("true" -> "1", "0x10" -> "16", "200" -> "127")
  char text[CCU_VALUE_TEXT_SIZE];
  value = valueText(def, parsed, value, text);

  // An alias ("timecode_source_0") is echoed with the canonical key
  char canonKey[BMD_KEY_SIZE];
  if (alias) {
    memcpy(canonKey, paramKey, keyLen);
    canonKey[keyLen] = '\0';
    paramKey = canonKey;
  }

  // Broadcast change to connected TCP clients (Companion); already validated
  CCUBroadcast::sendParamChange(targetCameraId, paramKey, value, true);

  // Send SSE event to web client (all cameras, real-time)
  WebServer::sendSSEEvent(targetCameraId, paramKey, value);

  return true;
}
