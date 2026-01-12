/*
 * bmd_params.h
 * Blackmagic camera parameter definitions
 * Version 3.0
 * 
 * Based on Blackmagic Camera Control Protocol
 */

#ifndef BMD_PARAMS_H
#define BMD_PARAMS_H

#include <Arduino.h>

// Parameter data types for BMD protocol
enum ParamType {
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

// CCU parameter descriptor structure
struct BMDParamDef {
    uint8_t   groupId;
    uint8_t   paramId;
    ParamType paramType;
    const char* paramKey;
    bool      hasSubIndices;
    uint8_t   subCount;
    uint8_t   subIndex;
};

// Maximum subCount in current table (for optimizing temporary arrays)
#define BMD_MAX_SUB_COUNT 8

// Defined in bmd_params.cpp
extern BMDParamDef bmdParams[];
extern const int BMD_PARAM_COUNT;

#endif // BMD_PARAMS_H
