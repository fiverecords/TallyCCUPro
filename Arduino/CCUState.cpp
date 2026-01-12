/*
 * CCUState.cpp
 * CCU parameter state storage implementation
 * Version 3.0
 * 
 * Approximate RAM usage:
 * - 26 params * (24 + 32) bytes = 1456 bytes
 * - Additional variables: ~10 bytes
 * - Total: ~1470 bytes
 */

#include "CCUState.h"

// Static variable initialization
CCUState::ParamEntry CCUState::_params[CCU_STATE_MAX_PARAMS];
int CCUState::_paramCount = 0;
int CCUState::_currentCamera = 1;

void CCUState::begin() {
    clear();
    _currentCamera = 1;
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
    
    int idx = findParam(paramKey);
    
    if (idx >= 0) {
        // Update existing value
        strncpy(_params[idx].value, value, CCU_STATE_VALUE_SIZE - 1);
        _params[idx].value[CCU_STATE_VALUE_SIZE - 1] = '\0';
    } else if (_paramCount < CCU_STATE_MAX_PARAMS) {
        // Add new
        strncpy(_params[_paramCount].key, paramKey, CCU_STATE_KEY_SIZE - 1);
        _params[_paramCount].key[CCU_STATE_KEY_SIZE - 1] = '\0';
        strncpy(_params[_paramCount].value, value, CCU_STATE_VALUE_SIZE - 1);
        _params[_paramCount].value[CCU_STATE_VALUE_SIZE - 1] = '\0';
        _paramCount++;
    }
    // If full and doesn't exist, ignore
}

int CCUState::findParam(const char* paramKey) {
    for (int i = 0; i < _paramCount; i++) {
        if (strcmp(_params[i].key, paramKey) == 0) {
            return i;
        }
    }
    return -1;
}

void CCUState::clear() {
    _paramCount = 0;
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
