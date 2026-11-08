/*
 * VmixConnector.cpp
 * vMix connection implementation - Ultra-fast without String
 *
 * Features tally debounce to avoid flickering
 */

#include "VmixConnector.h"
#include "Network.h"
#include "NetUtils.h"
#include "SafeMode.h"

// Uncomment below to enable debug
//#define DEBUG_VMIX

// W5x00 socket status register values (Sn_SR)
#define VMIX_SNSR_ESTABLISHED 0x17
#define VMIX_SNSR_CLOSE_WAIT  0x1C

// Static variable initialization
EthernetClient VmixConnector::_vmixClient;
byte VmixConnector::_vmixip[4] = {DEFAULT_VMIX_IP};
bool VmixConnector::_connectEnabled = true;

// vMix input states packed 2 bits per input (MEM-05): 0 off, 1 program,
// 2 preview. n = inputs in the line; n == 0 means no vMix state.
struct TallyFrame {
  uint8_t n;
  uint8_t bits[(MAX_VMIX_INPUTS * 2 + 7) / 8];
};

static uint8_t frameGet(const TallyFrame& f, uint8_t i) {
  return (f.bits[i >> 2] >> ((i & 3) * 2)) & 3;
}

static bool frameEqual(const TallyFrame& a, const TallyFrame& b) {
  return memcmp(&a, &b, sizeof(TallyFrame)) == 0;
}

// Last applied vMix state and debounce candidate
static TallyFrame appliedTally = {0, {0}};
static TallyFrame pendingTally = {0, {0}};

// Line being received. Kept between calls (TAL-10): a TALLY line split
// across TCP segments or loop passes is only used once its '\n' arrives.
// Only the data after "TALLY OK " is stored; inputs above MAX_VMIX_INPUTS
// are consumed and not evaluated. Other lines are skipped.
static TallyFrame rxFrame;
enum : uint8_t { RX_PREFIX, RX_DATA, RX_SKIP };
static uint8_t rxState = RX_PREFIX;
static uint8_t rxPos = 0;               // prefix chars matched, then data chars

// Debounce variables
static unsigned long pendingTallyTime = 0;
static bool hasPendingTally = false;
const unsigned long TALLY_DEBOUNCE_MS = 50;

// Connection state
static uint16_t ownPort = 0;            // local port of our connection
static unsigned long lastRxMs = 0;      // last byte from vMix
static unsigned long lastTxMs = 0;      // last TALLY request queued
static unsigned long probeStartMs = 0;  // first unanswered TALLY request
static bool probing = false;
static bool answered = false;           // vMix sent something on this connection
static bool wasConnected = false;
static bool connectNow = false;         // attempt on the next pass
static uint8_t failCount = 0;           // consecutive failed attempts
static unsigned long nextAttemptMs = 0;
static bool busy = false;               // processData() or pollTally() running

static const char TALLY_PREFIX[] PROGMEM = "TALLY OK ";
#define TALLY_PREFIX_LEN 9
// SUBSCRIBE for the changes, TALLY for the current state
static const char SUBSCRIBE_CMD[] PROGMEM = "SUBSCRIBE TALLY\r\nTALLY\r\n";
static const char TALLY_CMD[] PROGMEM = "TALLY\r\n";

// Wait (s) after the 1st, 2nd... consecutive failed attempt (TAL-06)
static const uint8_t RETRY_S[] PROGMEM = {2, 2, 5, 10, 20, VMIX_RECONNECT_MAX_MS / 1000};

static void printVmixIP(const byte ip[4]) {
  for (byte i = 0; i < 4; i++) {
    if (i) Serial.print('.');
    Serial.print(ip[i]);
  }
}

// Queues a PROGMEM command without waiting; false if it does not fit now
static bool sendCommand(EthernetClient& c, PGM_P cmd, size_t len) {
  char buf[sizeof(SUBSCRIBE_CMD)];
  if (len >= sizeof(buf)) return false;
  memcpy_P(buf, cmd, len);
  return NetUtils::tryWriteAll(c, (const uint8_t*)buf, len);
}

#if defined(DEBUG_VMIX) || DEBUG_HOT
static void printFrame(const TallyFrame& f) {
  for (uint8_t i = 0; i < f.n; i++) Serial.print(frameGet(f, i));
  Serial.println();
}
#endif

// Turn every tally off and forget the last vMix state
static void clearTally() {
  TallyManager::clearAll();
  appliedTally.n = 0;
  hasPendingTally = false;
}

// TAL-11: with a static IP the library starts every boot at local port
// 49153, the port of the connection a vMix PC may still think is open after
// our reset. Each EthernetUDP begin(0) takes the next port, so a few of
// them make the first connect use another one.
static void skipLocalPorts(uint8_t n) {
  EthernetUDP udp;
  for (uint8_t i = 0; i < n; i++) {
    if (!udp.begin(0)) break;   // no chip or no free socket
  }
  udp.stop();
}

bool VmixConnector::begin(const byte vmixip[4]) {
  memcpy(_vmixip, vmixip, 4);

  bool dummyTally, dummyCCU;
  StorageManager::loadOverrides(dummyTally, dummyCCU, _connectEnabled);

  Serial.print(F("vMix connection state loaded: "));
  Serial.println(_connectEnabled ? F("ON") : F("OFF"));

  skipLocalPorts(1 + (SafeMode::getBootSeed() & 63));

  return connect();
}

bool VmixConnector::connect() {
  if (!_connectEnabled) {
    Serial.println(F("vMix connection disabled by user"));
    return false;
  }
  connectNow = true;
  failCount = 0;
  return true;
}

// Connects with a short timeout, then subscribes. 1 connected, 0 refused or
// no answer, -1 no free socket (not counted as a vMix failure). A refused
// connect() returns with _sockindex still set on a socket that is already
// free: it is forgotten here before another module can take the index.
int8_t VmixConnector::openSocket() {
  closeSocket(true);

  // connect() also returns at once without a socket for these: a normal
  // failure (log, backoff), not "no free socket"
  if ((_vmixip[0] | _vmixip[1] | _vmixip[2] | _vmixip[3]) == 0 ||
      (_vmixip[0] & _vmixip[1] & _vmixip[2] & _vmixip[3]) == 255) {
    return 0;
  }

  IPAddress vmixIPAddress(_vmixip[0], _vmixip[1], _vmixip[2], _vmixip[3]);
  _vmixClient.setConnectionTimeout(VMIX_CONNECT_TIMEOUT_MS);

  unsigned long t0 = millis();
  if (!_vmixClient.connect(vmixIPAddress, VMIX_PORT)) {
    bool refused = _vmixClient.getSocketNumber() < MAX_SOCK_NUM;
    dropSocket();
    if (refused || millis() - t0 >= VMIX_CONNECT_TIMEOUT_MS) return 0;
    return -1;
  }
  ownPort = _vmixClient.localPort();

  if (!sendCommand(_vmixClient, SUBSCRIBE_CMD, sizeof(SUBSCRIBE_CMD) - 1)) {
    closeSocket(false);
    return 0;
  }

  // Fresh line parser; apply the state vMix sends now
  unsigned long now = millis();
  lastRxMs = now;
  lastTxMs = now;
  probeStartMs = now;
  probing = true;
  answered = false;
  rxState = RX_PREFIX;
  rxPos = 0;
  appliedTally.n = 0;
  hasPendingTally = false;
  return 1;
}

// True only while _vmixClient holds our own connection to vMix. A socket that
// is closed or was reused by HTTP/Companion is forgotten WITHOUT closing it,
// which would close a connection that belongs to someone else.
bool VmixConnector::ownsSocket() {
  if (_vmixClient.getSocketNumber() >= MAX_SOCK_NUM) return false;

  uint8_t s = _vmixClient.status();
  if ((s == VMIX_SNSR_ESTABLISHED || s == VMIX_SNSR_CLOSE_WAIT) &&
      _vmixClient.localPort() == ownPort &&
      _vmixClient.remotePort() == VMIX_PORT &&
      _vmixClient.remoteIP() == _vmixip) {
    return true;
  }

  dropSocket();
  return false;
}

// Forget the socket index without touching the W5500
void VmixConnector::dropSocket() {
  _vmixClient = EthernetClient();
}

// Close our own connection, or just forget a socket that is not ours.
// graceful: FIN, bounded wait for vMix to answer it. Otherwise closed at once
// (dead peer: nothing to wait for).
void VmixConnector::closeSocket(bool graceful) {
  if (!ownsSocket()) return;
  if (graceful) {
    NetUtils::closeGracefully(_vmixClient, VMIX_CLOSE_TIMEOUT_MS);
  } else {
    NetUtils::closeNow(_vmixClient);
  }
}

// The connection is gone: tally off (TAL-05) and first new attempt at once
void VmixConnector::connectionLost(bool graceful) {
  closeSocket(graceful);
  clearTally();
  wasConnected = false;
  connectNow = true;
}

bool VmixConnector::isConnected() {
  return ownsSocket() && _vmixClient.connected() && answered &&
         (millis() - lastRxMs < VMIX_RX_TIMEOUT_MS);
}

// Reads at most a few hundred bytes of what has already arrived; never waits
void VmixConnector::readInput(unsigned long now) {
  uint8_t chunk[32];
  int budget = 256;

  while (budget > 0) {
    int n = _vmixClient.available();
    if (n <= 0) break;
    if (n > (int)sizeof(chunk)) n = sizeof(chunk);
    if (n > budget) n = budget;
    n = _vmixClient.read(chunk, n);
    if (n <= 0) break;
    budget -= n;

    // Any byte proves the link is alive (TAL-03)
    lastRxMs = now;
    probing = false;
    answered = true;

    for (int i = 0; i < n; i++) {
      char ch = (char)chunk[i];
      if (ch == '\n') {
        if (rxState == RX_DATA && rxPos > 0) {
          rxFrame.n = rxPos;
          onTallyLine(rxFrame, now);
        }
        rxState = RX_PREFIX;
        rxPos = 0;
      } else if (ch == '\r') {
        // CRLF: the line ends at '\n'
      } else if (rxState == RX_PREFIX) {
        if (ch == (char)pgm_read_byte(&TALLY_PREFIX[rxPos])) {
          if (++rxPos == TALLY_PREFIX_LEN) {
            rxState = RX_DATA;
            rxPos = 0;
            memset(&rxFrame, 0, sizeof(rxFrame));
          }
        } else {
          rxState = RX_SKIP;
        }
      } else if (rxState == RX_DATA) {
        if (rxPos < MAX_VMIX_INPUTS) {
          // '1' program, '2' preview, anything else off
          uint8_t st = (ch == '1') ? 1 : (ch == '2') ? 2 : 0;
          rxFrame.bits[rxPos >> 2] |= (uint8_t)(st << ((rxPos & 3) * 2));
          rxPos++;
        }
      }
    }
  }
}

// Application heartbeat (TAL-03): vMix only sends on changes and the W5500
// has no keepalive, so a vMix that died without FIN looked connected forever.
// After VMIX_HEARTBEAT_MS without traffic a TALLY request is queued; vMix
// answers "TALLY OK ...". The dead-link timer starts at the first request,
// so a loop that stalled for a while does not count as silence from vMix.
void VmixConnector::heartbeat(unsigned long now) {
  if (now - lastRxMs < VMIX_HEARTBEAT_MS || now - lastTxMs < VMIX_HEARTBEAT_MS) return;

  if (!probing) {
    probing = true;
    probeStartMs = now;
  }
  // Socket full or busy: retried on the next pass, the timer keeps running
  if (sendCommand(_vmixClient, TALLY_CMD, sizeof(TALLY_CMD) - 1)) {
    lastTxMs = now;
  }
}

// DEBOUNCE: always follow the latest data received
void VmixConnector::onTallyLine(const TallyFrame& data, unsigned long now) {
  #ifdef DEBUG_VMIX
  Serial.print(F("[vmixdata] Received: "));
  printFrame(data);
  #endif

  if (frameEqual(data, appliedTally)) {
    hasPendingTally = false; // Back to the applied state
  } else if (!hasPendingTally || !frameEqual(data, pendingTally)) {
    pendingTally = data;
    pendingTallyTime = now;
    hasPendingTally = true;
  }
}

// Apply the pending state after the debounce. Tally is I2C only and never
// touches the SD card, so it also runs while the SD lock is held.
void VmixConnector::applyPending(unsigned long now) {
  if (!hasPendingTally || now - pendingTallyTime < TALLY_DEBOUNCE_MS) return;

  applyTallyDirectly(pendingTally);
  appliedTally = pendingTally;
  hasPendingTally = false;

  #if DEBUG_HOT
  Serial.print(F("[VMIX] "));
  printFrame(appliedTally);
  #endif
}

// Map or override changed (TAL-09): show the current vMix state again.
// While override was off the data kept arriving into appliedTally.
void VmixConnector::reapply() {
  if (appliedTally.n != 0) {
    applyTallyDirectly(appliedTally);
  } else {
    TallyManager::clearAll();
  }
}

void VmixConnector::tryReconnect(unsigned long now) {
  if (!connectNow && (long)(now - nextAttemptMs) < 0) return;
  connectNow = false;

  int8_t r = openSocket();
  now = millis();

  if (r > 0) {
    failCount = 0;
    wasConnected = true;
    Serial.print(F("vMix connected: "));
    printVmixIP(_vmixip);
    Serial.println();
    return;
  }

  if (r < 0) {
    // No free W5500 socket right now: not vMix's fault, retry soon
    nextAttemptMs = now + 1000UL;
    return;
  }

  if (failCount < 255) failCount++;
  uint8_t idx = failCount - 1;
  if (idx >= sizeof(RETRY_S)) idx = sizeof(RETRY_S) - 1;
  uint8_t waitS = pgm_read_byte(&RETRY_S[idx]);
  nextAttemptMs = now + (unsigned long)waitS * 1000UL;

  Serial.print(F("vMix connect failed ("));
  printVmixIP(_vmixip);
  Serial.print(F("), retry in "));
  Serial.print(waitS);
  Serial.println(F(" s"));
}

// Read, heartbeat and loss detection, shared by processData() and
// pollTally() so a loss also turns the tally off during a long file transfer
// (TAL-03/TAL-05). True while the link is up; reconnecting is left to
// processData(). mayWait: after vMix's FIN, close with a bounded FIN wait.
bool VmixConnector::serviceLink(unsigned long now, bool mayWait) {
  // Also drops a socket that is no longer ours before reading from it
  if (!ownsSocket()) {
    if (wasConnected) {
      // Reset by vMix, or the index already belongs to someone else
      Serial.println(F("vMix connection lost"));
      connectionLost(false);
    }
    return false;
  }

  readInput(now);
  heartbeat(now);

  if (!_vmixClient.connected()) {
    // vMix closed its side and everything has been read
    Serial.println(F("vMix connection lost"));
    connectionLost(mayWait);
    return false;
  }
  if (probing &&
      now - probeStartMs >= VMIX_RX_TIMEOUT_MS - VMIX_HEARTBEAT_MS &&
      now - lastRxMs >= VMIX_RX_TIMEOUT_MS) {
    // Half-open: vMix PC crashed, rebooted or lost the network (TAL-03)
    Serial.println(F("vMix not answering, reconnecting"));
    connectionLost(false);
    return false;
  }
  return true;
}

void VmixConnector::processData() {
  static bool disabledCleared = false;

  if (busy) return;
  busy = true;

  unsigned long now = millis();

  if (TallyManager::takeReapply()) {
    reapply();
  }

  if (!_connectEnabled) {
    closeSocket(true);
    wasConnected = false;
    // Turn tally off once when entering the disabled state
    if (!disabledCleared) {
      clearTally();
      disabledCleared = true;
    }
    busy = false;
    return;
  }
  disabledCleared = false;

  bool up = serviceLink(now, true);

  applyPending(now);

  if (!up) {
    tryReconnect(now);
  }

  busy = false;
}

void VmixConnector::pollTally() {
  if (busy || !_connectEnabled) return;
  if (!wasConnected && !hasPendingTally) return;
  busy = true;

  // A loss closes at once (no FIN wait inside a transfer loop)
  unsigned long now = millis();
  serviceLink(now, false);
  applyPending(now);

  busy = false;
}

bool VmixConnector::setVmixIP(const byte vmixip[4]) {
  // BOOT-11: a host address, not ours and not our network or broadcast
  // address. Outside the local subnet is allowed (routed), with a warning.
  IPAddress ip(vmixip);
  if (!NetworkManager::isHostIP(vmixip) || ip == NetworkManager::getLocalIP() ||
      NetworkManager::isLocalNetOrBroadcast(vmixip)) {
    Serial.print(F("Error: not a usable vMix address: "));
    printVmixIP(vmixip);
    Serial.println();
    return false;
  }

  // Close the old connection while the old IP still identifies it as ours
  closeSocket(true);
  wasConnected = false;

  memcpy(_vmixip, vmixip, 4);
  StorageManager::saveVmixIP(vmixip);

  Serial.print(F("vMix IP saved: "));
  printVmixIP(vmixip);
  Serial.println();
  if (!NetworkManager::inLocalSubnet(vmixip)) {
    Serial.println(F("Warning: vMix IP is outside the local subnet (needs the gateway)"));
  }

  // The old vMix state no longer applies
  clearTally();

  // Attempted on the next processData() pass, outside the HTTP handler
  if (_connectEnabled) connect();
  return true;
}

void VmixConnector::getVmixIP(byte vmixip[4]) {
  memcpy(vmixip, _vmixip, 4);
}

void VmixConnector::setConnectEnabled(bool enabled) {
  _connectEnabled = enabled;

  bool currentTally, currentCCU, currentVmix;
  StorageManager::loadOverrides(currentTally, currentCCU, currentVmix);
  StorageManager::saveOverrides(currentTally, currentCCU, _connectEnabled);

  Serial.print(F("vMix connection state changed to: "));
  Serial.println(_connectEnabled ? F("ON") : F("OFF"));

  if (!enabled) {
    bool wasUp = ownsSocket();
    closeSocket(true);
    wasConnected = false;
    if (wasUp) {
      Serial.println(F("vMix connection closed by user"));
    }
    clearTally();
  } else if (!ownsSocket()) {
    Serial.println(F("vMix connection enabled, attempting to connect..."));
    connect();
  }
}

bool VmixConnector::getConnectEnabled() {
  return _connectEnabled;
}

void VmixConnector::closeForNetworkChange() {
  if (ownsSocket()) {
    Serial.println(F("vMix connection closed (network change)"));
    closeSocket(true);
    clearTally();
  }
  wasConnected = false;
}

void VmixConnector::reconnectNow() {
  if (_connectEnabled) {
    connectNow = true;
    failCount = 0;
  }
}

void VmixConnector::applyTallyDirectly(const TallyFrame& tallyData) {
  #ifdef DEBUG_VMIX
  Serial.print(F("[VMIX] Data: "));
  printFrame(tallyData);
  #endif

  bool states[MAX_CAMERAS + 1][2] = {{false}};

  // Walk the map slots, not the line: any input 1..MAX_VMIX_INPUTS works and
  // several inputs on one camera are OR-ed instead of overwriting each other.
  // An empty slot reads as 0/0 and is skipped (TAL-07).
  for (byte slot = 0; slot < MAXTALLIES; slot++) {
    byte input, cameraId;
    TallyManager::getMapping(slot, input, cameraId);

    if (input < 1 || input > tallyData.n || cameraId < 1 || cameraId > MAX_CAMERAS) continue;

    uint8_t state = frameGet(tallyData, input - 1);
    if (state == 1) states[cameraId][0] = true;       // Program
    else if (state == 2) states[cameraId][1] = true;  // Preview
  }

  // Program wins over preview
  for (byte camera = 1; camera <= MAX_CAMERAS; camera++) {
    if (states[camera][0]) states[camera][1] = false;
  }

  // Always apply - setTallyStates has its own internal cache
  // that only sends I2C commands for cameras that actually changed
  TallyManager::setTallyStates(states);
}
