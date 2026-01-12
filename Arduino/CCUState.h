/*
 * CCUState.h
 * CCU parameter state storage for synchronization
 * Version 3.0
 * 
 * Stores last applied values so web interface can query them
 * via the /?getParams= endpoint
 * 
 * Limitations:
 * - Only stores active camera state (to save RAM)
 * - Maximum 26 parameters
 * - Only stores value as string (not parsed)
 */

#ifndef CCU_STATE_H
#define CCU_STATE_H

#include <Arduino.h>
#include <Ethernet.h>

// Maximum parameters to store
#define CCU_STATE_MAX_PARAMS 26

// Maximum parameter name size
#define CCU_STATE_KEY_SIZE 24

// Maximum value size
#define CCU_STATE_VALUE_SIZE 32

class CCUState {
public:
    static void begin();
    
    // Set current camera (clears state if changed)
    static void setCurrentCamera(int cameraId);
    static int getCurrentCamera();
    
    // Store a parameter value
    static void storeValue(const char* paramKey, const char* value);
    
    // Write state as JSON to client
    static void writeStateAsJSON(EthernetClient& client, int requestedCameraId);
    
    // Clear all state
    static void clear();
    
    // Get stored parameter count
    static int getParamCount();

private:
    struct ParamEntry {
        char key[CCU_STATE_KEY_SIZE];
        char value[CCU_STATE_VALUE_SIZE];
    };
    
    static ParamEntry _params[CCU_STATE_MAX_PARAMS];
    static int _paramCount;
    static int _currentCamera;
    
    static int findParam(const char* paramKey);
};

#endif // CCU_STATE_H
