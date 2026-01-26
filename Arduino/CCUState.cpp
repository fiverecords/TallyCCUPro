/*
 * CCUState.cpp
 * CCU parameter state storage implementation
 * Version 3.4
 * 
 * Approximate RAM usage:
 * - 10 params * (24 + 32 + 2) bytes = 580 bytes
 * - Additional variables: ~14 bytes
 * - Total: ~594 bytes
 * 
 * LRU Strategy:
 * - Each entry has accessOrder timestamp
 * - When full, find entry with lowest accessOrder
 * - Replace that entry with new parameter
 */

#include "CCUState.h"

// Static variable initialization
CCUState::ParamEntry CCUState::_params[CCU_STATE_MAX_PARAMS];
int CCUState::_paramCount = 0;
int CCUState::_currentCamera = 1;
uint16_t CCUState::_accessCounter = 0;

void CCUState::begin() {
    clear();
    _currentCamera = 1;
    _accessCounter = 0;
}

void CCUState::setCurrentCamera(int cameraId) {
    if (cameraId != _currentCamera) {
        clear();
        _currentCamera = cameraId;
    }
}

int CCUState::getCurrentCamera() {
    return _currentCamera;
}

void CCUState::storeValue(const char* paramKey, const char* value) {
    if (!paramKey || !value) return;
    
    // Increment access counter (with overflow protection)
    _accessCounter++;
    if (_accessCounter == 0) _accessCounter = 1;  // Avoid 0 after overflow
    
    int idx = findParam(paramKey);
    
    if (idx >= 0) {
        // Update existing value and access time
        strncpy(_params[idx].value, value, CCU_STATE_VALUE_SIZE - 1);
        _params[idx].value[CCU_STATE_VALUE_SIZE - 1] = '\0';
        _params[idx].accessOrder = _accessCounter;
    } else if (_paramCount < CCU_STATE_MAX_PARAMS) {
        // Add new entry (space available)
        strncpy(_params[_paramCount].key, paramKey, CCU_STATE_KEY_SIZE - 1);
        _params[_paramCount].key[CCU_STATE_KEY_SIZE - 1] = '\0';
        strncpy(_params[_paramCount].value, value, CCU_STATE_VALUE_SIZE - 1);
        _params[_paramCount].value[CCU_STATE_VALUE_SIZE - 1] = '\0';
        _params[_paramCount].accessOrder = _accessCounter;
        _paramCount++;
    } else {
        // Full - use LRU replacement
        int lruIdx = findLRUSlot();
        strncpy(_params[lruIdx].key, paramKey, CCU_STATE_KEY_SIZE - 1);
        _params[lruIdx].key[CCU_STATE_KEY_SIZE - 1] = '\0';
        strncpy(_params[lruIdx].value, value, CCU_STATE_VALUE_SIZE - 1);
        _params[lruIdx].value[CCU_STATE_VALUE_SIZE - 1] = '\0';
        _params[lruIdx].accessOrder = _accessCounter;
    }
}

int CCUState::findParam(const char* paramKey) {
    for (int i = 0; i < _paramCount; i++) {
        if (strcmp(_params[i].key, paramKey) == 0) {
            return i;
        }
    }
    return -1;
}

int CCUState::findLRUSlot() {
    int minIdx = 0;
    uint16_t minOrder = _params[0].accessOrder;
    
    for (int i = 1; i < _paramCount; i++) {
        if (_params[i].accessOrder < minOrder) {
            minOrder = _params[i].accessOrder;
            minIdx = i;
        }
    }
    
    return minIdx;
}

void CCUState::clear() {
    _paramCount = 0;
    _accessCounter = 0;
}

int CCUState::getParamCount() {
    return _paramCount;
}

void CCUState::writeStateAsJSON(EthernetClient& client, int requestedCameraId) {
    // HTTP headers
    client.println(F("HTTP/1.1 200 OK"));
    client.println(F("Content-Type: application/json"));
    client.println(F("Access-Control-Allow-Origin: *"));
    client.println(F("Cache-Control: no-cache"));
    client.println(F("Connection: close"));
    client.println();
    
    // Return empty if requested camera is not current
    if (requestedCameraId != _currentCamera || _paramCount == 0) {
        client.print(F("{\"cameraId\":"));
        client.print(requestedCameraId);
        client.println(F(",\"paramCount\":0,\"params\":{}}"));
        return;
    }
    
    // Write JSON with parameters
    client.print(F("{\"cameraId\":"));
    client.print(_currentCamera);
    client.print(F(",\"paramCount\":"));
    client.print(_paramCount);
    client.print(F(",\"params\":{"));
    
    for (int i = 0; i < _paramCount; i++) {
        if (i > 0) client.print(',');
        
        client.print('"');
        client.print(_params[i].key);
        client.print(F("\":"));
        
        // Determine if value is numeric or needs quotes
        const char* val = _params[i].value;
        bool isNumeric = true;
        bool hasComma = false;
        
        for (const char* p = val; *p; p++) {
            if (*p == ',') {
                hasComma = true;
            } else if (*p != '-' && *p != '.' && (*p < '0' || *p > '9')) {
                isNumeric = false;
                break;
            }
        }
        
        if (hasComma) {
            // Array - convert "1,2,3" to [1,2,3]
            client.print('[');
            client.print(val);
            client.print(']');
        } else if (isNumeric && val[0] != '\0') {
            // Simple number
            client.print(val);
        } else {
            // String
            client.print('"');
            client.print(val);
            client.print('"');
        }
    }
    
    client.println(F("}}"));
}
