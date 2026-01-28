/*
 * CCUBroadcast.cpp
 * TCP server for CCU change broadcasting
 * Version 3.6
 */

#include "CCUBroadcast.h"
#include "WebServer.h"
#include <avr/wdt.h>

// Static variable initialization
EthernetServer CCUBroadcast::_server(CCU_BROADCAST_PORT);
EthernetClient CCUBroadcast::_clients[CCU_MAX_CLIENTS];
bool CCUBroadcast::_subscribed[CCU_MAX_CLIENTS] = {false};
unsigned long CCUBroadcast::_lastActivity[CCU_MAX_CLIENTS] = {0};
char CCUBroadcast::_msgBuffer[CCU_MSG_BUFFER_SIZE];

// Static buffer for command reading
static char cmdBuffer[48];

bool CCUBroadcast::begin() {
    for (int i = 0; i < CCU_MAX_CLIENTS; i++) {
        _subscribed[i] = false;
        _lastActivity[i] = 0;
    }
    
    _server.begin();
    
    Serial.print(F("[CCUBcast] Port "));
    Serial.println(CCU_BROADCAST_PORT);
    
    return true;
}

void CCUBroadcast::process() {
    unsigned long currentTime = millis();
    
    // STEP 1: Process EXISTING clients first
    // Important because available() can return existing clients
    for (int i = 0; i < CCU_MAX_CLIENTS; i++) {
        if (!_clients[i]) continue;
        
        // Check if still connected
        if (!_clients[i].connected()) {
            Serial.print(F("[CCUBcast] Client "));
            Serial.print(i);
            Serial.println(F(" disconnected"));
            cleanupClient(i);
            continue;
        }
        
        // Inactivity timeout
        if (currentTime - _lastActivity[i] > CCU_CLIENT_TIMEOUT) {
            Serial.print(F("[CCUBcast] Timeout client "));
            Serial.println(i);
            cleanupClient(i);
            continue;
        }
        
        // Read available data
        while (_clients[i].available()) {
            int len = 0;
            while (_clients[i].available() && len < 47) {
                char c = _clients[i].read();
                if (c == '\r' || c == '\n') {
                    if (len > 0) break;
                    continue;
                }
                cmdBuffer[len++] = c;
            }
            cmdBuffer[len] = '\0';
            
            if (len > 0) {
                _lastActivity[i] = currentTime;
                processClientCommand(i, cmdBuffer);
            }
        }
    }
    
    // STEP 2: Accept NEW connections
    // available() can return existing clients, must filter them
    EthernetClient newClient = _server.available();
    
    while (newClient) {
        // CRITICAL: Check if this client is already in our array
        bool isExistingClient = false;
        for (int i = 0; i < CCU_MAX_CLIENTS; i++) {
            if (_clients[i] && _clients[i] == newClient) {
                isExistingClient = true;
                break;
            }
        }
        
        if (isExistingClient) {
            break;
        }
        
        // Genuinely new connection
        IPAddress newClientIP = newClient.remoteIP();
        
        Serial.print(F("[CCUBcast] New connection from "));
        Serial.println(newClientIP);
        
        // Check if connection from this IP already exists (reconnection)
        int existingSlot = findClientByIP(newClientIP);
        if (existingSlot >= 0) {
            Serial.print(F("[CCUBcast] Closing previous connection slot "));
            Serial.println(existingSlot);
            cleanupClient(existingSlot);
        }
        
        // Find free slot
        int slot = findFreeSlot();
        if (slot >= 0) {
            _clients[slot] = newClient;
            _subscribed[slot] = false;
            _lastActivity[slot] = currentTime;
            
            Serial.print(F("[CCUBcast] Assigned slot "));
            Serial.println(slot);
            
            // Welcome message
            _clients[slot].println(F("CCUSERVER OK TallyCCU-Pro"));
        } else {
            Serial.println(F("[CCUBcast] No free slots"));
            newClient.println(F("ERROR Server full"));
            newClient.stop();
        }
        
        // Try to accept more pending connections
        newClient = _server.available();
    }
}

int CCUBroadcast::findClientByIP(IPAddress ip) {
    for (int i = 0; i < CCU_MAX_CLIENTS; i++) {
        if (_clients[i] && _clients[i].connected()) {
            if (_clients[i].remoteIP() == ip) {
                return i;
            }
        }
    }
    return -1;
}

void CCUBroadcast::processClientCommand(int clientIndex, const char* command) {
    Serial.print(F("[CCUBcast] ["));
    Serial.print(clientIndex);
    Serial.print(F("] "));
    Serial.println(command);
    
    // SUBSCRIBE - Subscribe to changes
    if (strncmp(command, "SUBSCRIBE", 9) == 0) {
        _subscribed[clientIndex] = true;
        sendToClient(clientIndex, "SUBSCRIBED OK");
        
        // Request sync from SSE client (web) to sync new TCP client
        if (WebServer::hasSSEClient()) {
            WebServer::sendSSERequestSync();
            Serial.println(F("[CCUBcast] Sent REQUESTSYNC to SSE client"));
        }
        return;
    }
    
    // UNSUBSCRIBE - Cancel subscription
    if (strncmp(command, "UNSUBSCRIBE", 11) == 0) {
        _subscribed[clientIndex] = false;
        sendToClient(clientIndex, "UNSUBSCRIBED OK");
        return;
    }
    
    // CCUSYNC - Sync message from Companion, forward to SSE
    if (strncmp(command, "CCUSYNC ", 8) == 0) {
        // Parse: CCUSYNC cameraId paramKey value
        const char* data = command + 8;
        int cameraId = atoi(data);
        
        // Find space after cameraId
        const char* p = data;
        while (*p && *p != ' ') p++;
        if (*p == ' ') p++;
        
        // Find paramKey
        const char* paramKey = p;
        while (*p && *p != ' ') p++;
        
        if (*p == ' ') {
            // Extract paramKey
            int keyLen = p - paramKey;
            if (keyLen > 0 && keyLen < 32) {
                char key[32];
                strncpy(key, paramKey, keyLen);
                key[keyLen] = '\0';
                
                const char* value = p + 1;
                
                // Forward to SSE client
                WebServer::sendSSEEvent(cameraId, key, value);
            }
        }
        return;
    }
    
    // PING - Keepalive
    if (strcmp(command, "PING") == 0) {
        sendToClient(clientIndex, "PONG");
        return;
    }
    
    // STATUS - Server status
    if (strcmp(command, "STATUS") == 0) {
        snprintf(_msgBuffer, CCU_MSG_BUFFER_SIZE, 
                 "STATUS slot=%d clients=%d sub=%d",
                 clientIndex,
                 getClientCount(),
                 _subscribed[clientIndex] ? 1 : 0);
        sendToClient(clientIndex, _msgBuffer);
        return;
    }
    
    // Unknown command
    sendToClient(clientIndex, "ERROR Unknown command");
}

void CCUBroadcast::sendToClient(int clientIndex, const char* message) {
    if (clientIndex < 0 || clientIndex >= CCU_MAX_CLIENTS) return;
    if (!_clients[clientIndex] || !_clients[clientIndex].connected()) return;
    
    _clients[clientIndex].println(message);
    _lastActivity[clientIndex] = millis();
}

void CCUBroadcast::broadcast(const char* message) {
    unsigned long currentTime = millis();
    for (int i = 0; i < CCU_MAX_CLIENTS; i++) {
        if (_clients[i] && _clients[i].connected() && _subscribed[i]) {
            _clients[i].println(message);
            _lastActivity[i] = currentTime;
        }
    }
}

void CCUBroadcast::requestSync() {
    if (!hasClients()) return;
    broadcast("REQUESTSYNC");
    Serial.println(F("[CCUBcast] Sent REQUESTSYNC"));
}

void CCUBroadcast::sendParamChange(int cameraId, const char* paramKey, const char* value) {
    if (!hasClients()) return;
    
    snprintf(_msgBuffer, CCU_MSG_BUFFER_SIZE, 
             "CCU %d %s %s", 
             cameraId, paramKey, value);
    
    broadcast(_msgBuffer);
}

void CCUBroadcast::sendPresetLoaded(int cameraId, int presetId, const char* presetName) {
    if (!hasClients()) return;
    
    snprintf(_msgBuffer, CCU_MSG_BUFFER_SIZE, 
             "PRESET %d %d %s", 
             cameraId, presetId, presetName ? presetName : "");
    
    broadcast(_msgBuffer);
}

void CCUBroadcast::sendPresetSaved(int cameraId, int presetId, const char* presetName) {
    if (!hasClients()) return;
    
    snprintf(_msgBuffer, CCU_MSG_BUFFER_SIZE, 
             "PRESETSAVED %d %d %s", 
             cameraId, presetId, presetName ? presetName : "");
    
    broadcast(_msgBuffer);
}

void CCUBroadcast::cleanupClient(int clientIndex) {
    if (clientIndex < 0 || clientIndex >= CCU_MAX_CLIENTS) return;
    
    if (_clients[clientIndex]) {
        _clients[clientIndex].stop();
        _clients[clientIndex] = EthernetClient();
    }
    _subscribed[clientIndex] = false;
    _lastActivity[clientIndex] = 0;
}

int CCUBroadcast::findFreeSlot() {
    for (int i = 0; i < CCU_MAX_CLIENTS; i++) {
        if (!_clients[i] || !_clients[i].connected()) {
            if (_clients[i]) {
                cleanupClient(i);
            }
            return i;
        }
    }
    return -1;
}

uint8_t CCUBroadcast::getClientCount() {
    uint8_t count = 0;
    for (int i = 0; i < CCU_MAX_CLIENTS; i++) {
        if (_clients[i] && _clients[i].connected()) {
            count++;
        }
    }
    return count;
}

bool CCUBroadcast::hasClients() {
    for (int i = 0; i < CCU_MAX_CLIENTS; i++) {
        if (_clients[i] && _clients[i].connected() && _subscribed[i]) {
            return true;
        }
    }
    return false;
}
