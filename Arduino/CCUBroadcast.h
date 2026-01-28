/*
 * CCUBroadcast.h
 * TCP server for CCU change broadcasting
 * Version 3.6
 * 
 * Protocol (outgoing messages):
 * - CCU <cameraId> <paramKey> <value>        - Parameter change
 * - PRESET <cameraId> <presetId> <name>      - Preset loaded
 * - PRESETSAVED <cameraId> <presetId> <name> - Preset saved
 * 
 * Commands (incoming):
 * - SUBSCRIBE     - Subscribe to changes
 * - UNSUBSCRIBE   - Cancel subscription
 * - STATUS        - Get status
 * - PING          - Keepalive
 */

#ifndef CCU_BROADCAST_H
#define CCU_BROADCAST_H

#include "Configuration.h"
#include <Ethernet.h>

#define CCU_BROADCAST_PORT 8098
#define CCU_MAX_CLIENTS 2
#define CCU_CLIENT_TIMEOUT 300000  // 5 minutes
#define CCU_MSG_BUFFER_SIZE 64

class CCUBroadcast {
public:
    static bool begin();
    static void process();
    
    // Push notifications
    static void sendParamChange(int cameraId, const char* paramKey, const char* value);
    static void sendPresetLoaded(int cameraId, int presetId, const char* presetName);
    static void sendPresetSaved(int cameraId, int presetId, const char* presetName);
    
    // Request sync from TCP clients (for SSE initial state)
    static void requestSync();
    
    // Status
    static uint8_t getClientCount();
    static bool hasClients();

private:
    static EthernetServer _server;
    static EthernetClient _clients[CCU_MAX_CLIENTS];
    static bool _subscribed[CCU_MAX_CLIENTS];
    static unsigned long _lastActivity[CCU_MAX_CLIENTS];
    static char _msgBuffer[CCU_MSG_BUFFER_SIZE];
    
    static void processClientCommand(int clientIndex, const char* command);
    static void sendToClient(int clientIndex, const char* message);
    static void broadcast(const char* message);
    static void cleanupClient(int clientIndex);
    static int findFreeSlot();
    static int findClientByIP(IPAddress ip);
};

#endif
