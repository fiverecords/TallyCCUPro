/*
 * CCUBroadcast.cpp
 * TCP server for CCU change broadcasting
 * Version 3.8
 */

#include "CCUBroadcast.h"
#include "webserver.h"
#include "bmd_params.h"
#include "CCUControl.h"
#include "NetUtils.h"
#include <utility/w5100.h>
#include <avr/wdt.h>
#include <avr/pgmspace.h>
#include <stdarg.h>

namespace {

// Per-client state. An EthernetClient is only a socket index: the peer
// stored at accept identifies the connection (see NetUtils.h).
struct Slot {
    uint32_t ip;
    uint16_t port;
    uint32_t lastRx;      // last line received (accept time until the first)
    uint32_t stallSince;  // first dropped line, 0 = TX healthy
    uint16_t drops;       // lines dropped since the stall began
    bool subscribed;
    bool talked;          // has sent at least one line
};

EthernetServer server(CCU_BROADCAST_PORT);
EthernetClient clients[CCU_MAX_CLIENTS];
Slot slots[CCU_MAX_CLIENTS];
char lineBuf[CCU_MAX_CLIENTS][CCU_LINE_BUFFER_SIZE];
static_assert(CCU_MAX_CLIENTS == 2, "readers[] initializer");
NetUtils::LineReader readers[CCU_MAX_CLIENTS] = {
    NetUtils::LineReader(lineBuf[0], CCU_LINE_BUFFER_SIZE),
    NetUtils::LineReader(lineBuf[1], CCU_LINE_BUFFER_SIZE)
};
char msg[CCU_MSG_BUFFER_SIZE];   // outgoing line, CRLF-terminated, no NUL
uint8_t rrFirst;                 // round robin of the read budget

// Sync rate limit (REQUESTSYNC to TCP, requestSync event to SSE)
uint32_t lastTcpSync, lastWebSync;
bool tcpSyncDone, webSyncDone, tcpSyncPending, webSyncPending;

const size_t POLL_BYTES = 2 * CCU_LINE_BUFFER_SIZE;  // a full line in one poll()

// ms elapsed since t, modulo 2^32 (also where unsigned long is 64-bit)
inline uint32_t since(uint32_t t) { return (uint32_t)millis() - t; }

void logClient(uint8_t i, const __FlashStringHelper* what) {
    Serial.print(F("[CCUBcast] Client "));
    Serial.print(i);
    Serial.print(' ');
    Serial.println(what);
}

// Socket status if it still holds the connection accepted into slot i
// (ESTABLISHED or CLOSE_WAIT), CLOSED otherwise.
uint8_t ownStatus(uint8_t i) {
    EthernetClient& c = clients[i];
    uint8_t st = c.status();
    if (st != SnSR::ESTABLISHED && st != SnSR::CLOSE_WAIT) return SnSR::CLOSED;
    if (c.localPort() != CCU_BROADCAST_PORT || c.remotePort() != slots[i].port ||
        (uint32_t)c.remoteIP() != slots[i].ip) {
        return SnSR::CLOSED;
    }
    return st;
}

// Frees slot i without touching the socket (it may belong to someone else)
void forget(uint8_t i) {
    clients[i] = EthernetClient();
    slots[i] = Slot();
    readers[i].reset();
}

// Closes slot i's connection if it is still ours, then frees the slot
void dropClient(uint8_t i, bool graceful, const __FlashStringHelper* why) {
    logClient(i, why);
    if (slots[i].drops) {
        Serial.print(F("[CCUBcast] Lines dropped: "));
        Serial.println(slots[i].drops);
    }
    if (ownStatus(i) != SnSR::CLOSED) {
        if (graceful) NetUtils::closeGracefully(clients[i], CCU_CLOSE_MS);
        else NetUtils::closeNow(clients[i]);
    }
    forget(i);
}

void txRecovered(uint8_t i) {
    Serial.print(F("[CCUBcast] Client "));
    Serial.print(i);
    Serial.print(F(" TX ok, lines dropped: "));
    Serial.println(slots[i].drops);
    slots[i].stallSince = 0;
    slots[i].drops = 0;
}

// Queues len bytes for client i without blocking. A line that does not fit
// (or a SEND still in flight after CCU_TX_WAIT_MS) is dropped for that
// client and counted. While stalled (until the TX buffer drains, see
// checkClient()) lines get a single attempt.
bool writeTo(uint8_t i, const char* data, size_t len) {
    if (!clients[i]) return false;
    if (ownStatus(i) == SnSR::CLOSED) {
        logClient(i, F("lost"));
        forget(i);
        return false;
    }
    Slot& s = slots[i];
    uint32_t t0 = millis();
    for (;;) {
        bool dead;
        if (NetUtils::tryWriteAll(clients[i], (const uint8_t*)data, len, &dead)) {
            return true;
        }
        if (dead) {
            dropClient(i, false, F("gone"));
            return false;
        }
        if (s.stallSince || since(t0) >= CCU_TX_WAIT_MS) break;
        wdt_reset();
    }
    if (!s.stallSince) {
        s.stallSince = millis() | 1;
        logClient(i, F("TX full, dropping lines"));
    }
    if (s.drops < 0xFFFF) s.drops++;
    return false;
}

// Formats msg + CRLF. Returns its length, 0 if it does not fit: a line is
// never sent truncated.
size_t formatMsg(PGM_P fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf_P(msg, CCU_MSG_BUFFER_SIZE - 2, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= CCU_MSG_BUFFER_SIZE - 2) {
        Serial.println(F("[CCUBcast] Line too long, not sent"));
        return 0;
    }
    msg[n] = '\r';
    msg[n + 1] = '\n';
    return (size_t)n + 2;
}

// Constant line (PROGMEM) + CRLF into msg
size_t constMsg(PGM_P text) {
    size_t n = strlen_P(text);
    if (n > CCU_MSG_BUFFER_SIZE - 2) n = CCU_MSG_BUFFER_SIZE - 2;
    memcpy_P(msg, text, n);
    msg[n] = '\r';
    msg[n + 1] = '\n';
    return n + 2;
}

void reply_P(uint8_t i, PGM_P text) {
    size_t n = constMsg(text);
    writeTo(i, msg, n);
}

void broadcastMsg(size_t len) {
    if (!len) return;
    for (uint8_t i = 0; i < CCU_MAX_CLIENTS; i++) {
        if (clients[i] && slots[i].subscribed) writeTo(i, msg, len);
    }
}

void sendTcpSync() {
    tcpSyncDone = true;
    tcpSyncPending = false;
    lastTcpSync = millis();
    broadcastMsg(constMsg(PSTR("REQUESTSYNC")));
    Serial.println(F("[CCUBcast] Sent REQUESTSYNC"));
}

// Asks the web client to push its state (POST /syncState) to TCP clients
void requestWebSync() {
    if (!WebServer::hasSSEClient()) return;
    if (webSyncDone && since(lastWebSync) < CCU_SYNC_INTERVAL) {
        webSyncPending = true;
        return;
    }
    webSyncDone = true;
    webSyncPending = false;
    lastWebSync = millis();
    WebServer::sendSSERequestSync();
    Serial.println(F("[CCUBcast] Sent REQUESTSYNC to SSE client"));
}

void handleCcuSync(const char* data) {
    // CCUSYNC <cameraId> <paramKey> <value>
    int cameraId = atoi(data);
    if (cameraId < 1 || cameraId > MAX_CAMERAS) return;
    const char* p = data;
    while (*p && *p != ' ') p++;
    if (*p == ' ') p++;
    const char* keyStart = p;
    while (*p && *p != ' ') p++;
    if (*p != ' ') return;

    char key[BMD_KEY_SIZE];
    size_t keyLen = p - keyStart;
    if (keyLen == 0 || keyLen >= sizeof(key)) return;
    memcpy(key, keyStart, keyLen);
    key[keyLen] = '\0';
    const char* value = p + 1;

    // An alias ("timecode_source_0") goes out unchanged, as in v3.7.1: it
    // is Companion's own entry, often stale, and must not overwrite the
    // canonical key (kept current by our CCU echo).
    BMDParamDef def;
    int8_t canon = bmdFindParam(key, &def);
    if (canon >= 0 && key[canon] == '\0') {
        // Actions are not state
        if (def.paramType == TYPE_VOID) return;
        if (def.paramType == TYPE_BOOLEAN) {
            // Same 1/0 form as applyParameterByKey broadcasts
            int8_t b = CCUControl::parseBool(value);
            if (b < 0) return;
            char bv[2] = { (char)('0' + b), '\0' };
            WebServer::sendSSEEvent(cameraId, key, bv);
            return;
        }
    }
    WebServer::sendSSEEvent(cameraId, key, value);
}

void processClientCommand(uint8_t i, const char* command) {
#if DEBUG_HOT
    Serial.print(F("[CCUBcast] ["));
    Serial.print(i);
    Serial.print(F("] "));
    Serial.println(command);
#endif

    // SUBSCRIBE - Subscribe to changes
    if (strncmp_P(command, PSTR("SUBSCRIBE"), 9) == 0) {
        slots[i].subscribed = true;
        reply_P(i, PSTR("SUBSCRIBED OK"));
        // Ask the web client (if any) to sync the new TCP client
        requestWebSync();
        return;
    }

    // UNSUBSCRIBE - Cancel subscription
    if (strncmp_P(command, PSTR("UNSUBSCRIBE"), 11) == 0) {
        slots[i].subscribed = false;
        reply_P(i, PSTR("UNSUBSCRIBED OK"));
        return;
    }

    // CCUSYNC - Sync message from Companion, forward to SSE
    if (strncmp_P(command, PSTR("CCUSYNC "), 8) == 0) {
        handleCcuSync(command + 8);
        return;
    }

    // PING - Keepalive
    if (strcmp_P(command, PSTR("PING")) == 0) {
        reply_P(i, PSTR("PONG"));
        return;
    }

    // STATUS - Server status
    if (strcmp_P(command, PSTR("STATUS")) == 0) {
        size_t n = formatMsg(PSTR("STATUS slot=%d clients=%d sub=%d"), (int)i,
                             (int)CCUBroadcast::getClientCount(),
                             slots[i].subscribed ? 1 : 0);
        if (n) writeTo(i, msg, n);
        return;
    }

    // Unknown command
    reply_P(i, PSTR("ERROR Unknown command"));
}

// Connection state and timeouts. Returns false if the slot was freed.
bool checkClient(uint8_t i) {
    uint8_t st = ownStatus(i);
    if (st == SnSR::CLOSED) {
        logClient(i, F("disconnected"));
        forget(i);
        return false;
    }
    Slot& s = slots[i];
    if (st == SnSR::CLOSE_WAIT && clients[i].available() == 0) {
        dropClient(i, true, F("closed by peer"));
        return false;
    }
    // Only received lines count as activity
    uint32_t silent = since(s.lastRx);
    if (!s.talked && silent >= CCU_HELLO_TIMEOUT) {
        dropClient(i, true, F("sent nothing"));
        return false;
    }
    if (silent >= CCU_CLIENT_TIMEOUT) {
        dropClient(i, true, F("timeout"));
        return false;
    }
    // A stall ends when the TX buffer has drained, not on a line that
    // happens to fit: a peer that only trickles is still closed.
    if (s.stallSince) {
        if (NetUtils::txIdle(clients[i]) &&
            clients[i].availableForWrite() >= CCU_TX_DRAINED) {
            txRecovered(i);
        } else if (since(s.stallSince) >= CCU_STALL_TIMEOUT) {
            dropClient(i, false, F("TX stalled"));
            return false;
        }
    }
    return true;
}

// Processes complete lines of client i, at most 'budget'. Unread data stays
// in the W5500 RX buffer for the next pass. Returns the budget left.
uint8_t readClient(uint8_t i, uint8_t budget) {
    while (budget > 0) {
        int n = readers[i].poll(clients[i], POLL_BYTES);
        if (n < 0) break;   // no complete line yet
        budget--;
        slots[i].lastRx = millis();
        slots[i].talked = true;
        if (readers[i].truncated()) {
            reply_P(i, PSTR("ERROR Line too long"));
        } else if (n > 0) {
            processClientCommand(i, lineBuf[i]);
        }
        if (!clients[i]) break;   // dropped while replying
        wdt_reset();
    }
    return budget;
}

int8_t freeSlot() {
    for (uint8_t i = 0; i < CCU_MAX_CLIENTS; i++) {
        if (!clients[i]) return (int8_t)i;
    }
    return -1;
}

// One new connection per pass, given a slot (and the banner) before it
// sends anything.
void acceptClient() {
    EthernetClient c = server.accept();
    if (!c) return;
    uint32_t ip = c.remoteIP();

    Serial.print(F("[CCUBcast] New connection from "));
    Serial.println(c.remoteIP());

    // Companion keeps one connection and PINGs every 30 s: a connection
    // from the same host that has been silent longer is its stale one.
    for (uint8_t i = 0; i < CCU_MAX_CLIENTS; i++) {
        if (clients[i] && slots[i].ip == ip &&
            since(slots[i].lastRx) >= CCU_REPLACE_SILENCE) {
            dropClient(i, true, F("replaced"));
        }
    }

    int8_t slot = freeSlot();
    if (slot < 0) {
        Serial.println(F("[CCUBcast] No free slots"));
        size_t n = constMsg(PSTR("ERROR Server full"));
        NetUtils::tryWriteAll(c, (const uint8_t*)msg, n);
        NetUtils::closeGracefully(c, CCU_CLOSE_MS);
        return;
    }

    clients[slot] = c;
    slots[slot] = Slot();
    slots[slot].ip = ip;
    slots[slot].port = c.remotePort();
    slots[slot].lastRx = millis();
    readers[slot].reset();

    Serial.print(F("[CCUBcast] Assigned slot "));
    Serial.println(slot);

    // Welcome message
    reply_P((uint8_t)slot, PSTR("CCUSERVER OK TallyCCU-Pro"));
}

void runPendingSyncs() {
    if (tcpSyncPending && since(lastTcpSync) >= CCU_SYNC_INTERVAL) {
        tcpSyncPending = false;
        if (CCUBroadcast::hasClients() && WebServer::hasSSEClient()) sendTcpSync();
    }
    if (webSyncPending && since(lastWebSync) >= CCU_SYNC_INTERVAL) {
        webSyncPending = false;
        if (CCUBroadcast::hasClients()) requestWebSync();
    }
}

}  // namespace

bool CCUBroadcast::begin() {
    for (uint8_t i = 0; i < CCU_MAX_CLIENTS; i++) {
        forget(i);
    }

    server.begin();

    Serial.print(F("[CCUBcast] Port "));
    Serial.println(CCU_BROADCAST_PORT);

    return true;
}

void CCUBroadcast::process() {
    // Existing clients: state, timeouts, then at most CCU_CMDS_PER_PASS
    // lines in total (round robin), with wdt_reset() between them
    uint8_t budget = CCU_CMDS_PER_PASS;
    uint8_t first = rrFirst;
    rrFirst = (uint8_t)((rrFirst + 1) % CCU_MAX_CLIENTS);
    for (uint8_t k = 0; k < CCU_MAX_CLIENTS; k++) {
        uint8_t i = (uint8_t)((first + k) % CCU_MAX_CLIENTS);
        if (!clients[i] || !checkClient(i)) continue;
        budget = readClient(i, budget);
    }

    acceptClient();
    runPendingSyncs();
}

void CCUBroadcast::requestSync() {
    if (!hasClients()) return;
    if (tcpSyncDone && since(lastTcpSync) < CCU_SYNC_INTERVAL) {
        tcpSyncPending = true;
        return;
    }
    sendTcpSync();
}

bool CCUBroadcast::syncSentNear(uint32_t t) {
    return tcpSyncDone && (int32_t)(t - lastTcpSync) < (int32_t)CCU_SYNC_INTERVAL;
}

void CCUBroadcast::sendParamChange(int cameraId, const char* paramKey, const char* value) {
    if (!hasClients()) return;
    // Actions are events, not state: clients must not cache them
    if (bmdIsVoidParam(paramKey)) return;

    broadcastMsg(formatMsg(PSTR("CCU %d %s %s"), cameraId, paramKey, value));
}

void CCUBroadcast::sendPresetLoaded(int cameraId, int presetId, const char* presetName) {
    if (!hasClients()) return;

    broadcastMsg(formatMsg(PSTR("PRESET %d %d %s"), cameraId, presetId,
                           presetName ? presetName : ""));
}

void CCUBroadcast::sendPresetSaved(int cameraId, int presetId, const char* presetName) {
    if (!hasClients()) return;

    broadcastMsg(formatMsg(PSTR("PRESETSAVED %d %d %s"), cameraId, presetId,
                           presetName ? presetName : ""));
}

// Slots in use. Their sockets are checked on every use and every process().
uint8_t CCUBroadcast::getClientCount() {
    uint8_t count = 0;
    for (uint8_t i = 0; i < CCU_MAX_CLIENTS; i++) {
        if (clients[i]) count++;
    }
    return count;
}

bool CCUBroadcast::hasClients() {
    for (uint8_t i = 0; i < CCU_MAX_CLIENTS; i++) {
        if (clients[i] && slots[i].subscribed) return true;
    }
    return false;
}
