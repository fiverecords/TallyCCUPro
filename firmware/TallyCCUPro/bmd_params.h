/*
 * bmd_params.h
 * Blackmagic camera parameter definitions
 *
 * Based on Blackmagic Camera Control Protocol
 */

#ifndef BMD_PARAMS_H
#define BMD_PARAMS_H

#include <Arduino.h>

// Parameter data types for BMD protocol
enum ParamType : uint8_t {
    TYPE_VOID,     // 0
    TYPE_BOOLEAN,  // 1
    TYPE_INT8,     // 2
    TYPE_INT16,    // 3
    TYPE_INT32,    // 4
    TYPE_INT64,    // 5
    TYPE_FIXED16,  // 6
    TYPE_STRING,   // 7
    TYPE_UNKNOWN,  // 8
    TYPE_UINT16    // 9 - For bit fields like exposure_and_focus_tools
};

// Longest key ("color_bars_display_time_seconds") + NUL
#define BMD_KEY_SIZE 32

// Row of the parameter table. The table lives in flash (PROGMEM) and is
// private to bmd_params.cpp: read it through bmdFindParam().
struct BMDParamRow {
    uint8_t   groupId;
    uint8_t   paramId;
    ParamType paramType;
    char      paramKey[BMD_KEY_SIZE];
    bool      hasSubIndices;   // same as subCount > 1
    uint8_t   subCount;
};

// RAM copy of a table row (without the key), filled by bmdFindParam()
struct BMDParamDef {
    uint8_t   groupId;
    uint8_t   paramId;
    ParamType paramType;
    uint8_t   subCount;        // > 1: comma-separated list
};

// Largest subCount in the table (checked at compile time in bmd_params.cpp).
// Lists are written as arrays of exactly subCount elements (CCU-08).
#define BMD_MAX_SUB_COUNT 5

// Longest TYPE_STRING value sent to a camera (system_language is a
// two-letter ISO 639-1 code)
#define BMD_STRING_MAX 16

extern const uint8_t BMD_PARAM_COUNT;

// Looks key up in the table. "<key>_0" is also accepted for a single-value
// <key> (Companion sends timecode_source_0). On success copies the row to
// *out (may be NULL) and returns the length of the canonical key, i.e. the
// leading part of key to use from then on (strlen(key) - 2 for the alias).
// Returns -1 if the key is unknown.
int8_t bmdFindParam(const char *key, BMDParamDef *out);

// True if key is a TYPE_VOID (action) parameter: an event, not state
bool bmdIsVoidParam(const char *paramKey);

#endif // BMD_PARAMS_H
