/*
 * CCUBroadcast.h
 * TCP server for CCU change broadcasting
 *
 * Protocol (outgoing messages):
 * - CCU <cameraId> <paramKey> <value>        - Parameter change
 * - PRESET <cameraId> <presetId> <name>      - Preset loaded
 * - PRESETSAVED <cameraId> <presetId> <name> - Preset saved
 * - REQUESTSYNC                              - Asks for cached state
 *
 * Commands (incoming, one per line ending in '\n'; '\r' is ignored):
 * - SUBSCRIBE     - Subscribe to changes (required within
 *                   CCU_HELLO_TIMEOUT of connecting)
 * - UNSUBSCRIBE   - Cancel subscription
 * - CCUSYNC <cameraId> <paramKey> <value> - Cached state, forwarded to SSE
 *                   if the camera, key and value are valid (value in the
 *                   form of the CCU echo, see CCUControl::canonicalValue())
 * - STATUS        - Get status
 * - PING          - Keepalive
 *
 * No authentication: Companion (v3.1.0) sends none. Outgoing lines never
 * contain control characters (TCP-07).
 */

#ifndef CCU_BROADCAST_H
#define CCU_BROADCAST_H

#include "Configuration.h"
#include <Ethernet.h>

#define CCU_BROADCAST_PORT 8098
#define CCU_MAX_CLIENTS 2
#define CCU_CLIENT_TIMEOUT 90000UL   // nothing received (Companion PINGs every 30 s)
#define CCU_HELLO_TIMEOUT 10000UL    // no SUBSCRIBE after connecting
#define CCU_REPLACE_SILENCE 35000UL  // same-IP client this silent: stale Companion connection
#define CCU_STALL_TIMEOUT 5000UL     // TX not drained this long after a drop: close
#define CCU_TX_DRAINED 1024          // free TX bytes that end a stall (half the buffer)
#define CCU_TX_WAIT_MS 5             // grace for a busy TX before a line is dropped
#define CCU_CLOSE_MS 20              // budget of a graceful close
#define CCU_SYNC_INTERVAL 10000UL    // REQUESTSYNC / web sync request at most this often
#define CCU_CMDS_PER_PASS 8          // lines processed per process() call
#define CCU_LINE_BUFFER_SIZE 128     // per client; longer lines are rejected
#define CCU_MSG_BUFFER_SIZE 136      // outgoing line incl. CRLF

// Optional allowlist, off by default: only these hosts may connect to port
// 8098 (e.g. the Companion machine), 4 bytes per host; others are closed
// at once. Companion needs no change.
//   #define CCU_ALLOWED_HOSTS 192,168,1,50, 192,168,1,51

class CCUBroadcast {
public:
    static bool begin();
    static void process();

    // Push notifications. Never block: a line that does not fit in a
    // client's TX buffer is dropped for that client and counted.
    // sendParamChange() drops actions and, unless validated (by
    // CCUControl), an invalid camera, key or value (as from web syncState);
    // such a value is sent as CCUControl::canonicalValue() gives it.
    // A validated value is sent as is (already in that form).
    static void sendParamChange(int cameraId, const char* paramKey, const char* value,
                                bool validated = false);
    static void sendPresetLoaded(int cameraId, int presetId, const char* presetName);
    static void sendPresetSaved(int cameraId, int presetId, const char* presetName);

    // Asks TCP clients for their cached state (REQUESTSYNC, for a new SSE
    // client). At most once per CCU_SYNC_INTERVAL: a request inside the
    // interval is deferred, not lost. Call it on every SSE connection.
    static void requestSync();

    // True if a REQUESTSYNC went out after t or less than CCU_SYNC_INTERVAL
    // before it: for SSE drops that began at t, its dump resends the values
    // or caused the drops, and asking again could loop.
    static bool syncSentNear(uint32_t t);

    // Status
    static uint8_t getClientCount();
    static bool hasClients();
};

#endif
