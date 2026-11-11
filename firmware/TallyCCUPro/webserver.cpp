/*
 * webserver.cpp
 * Web server for control and configuration
 */

#include "webserver.h"
#include "CCUBroadcast.h"
#include "SafeMode.h"
#include <SdFat.h>
#include <avr/wdt.h>
#include <utility/w5100.h>

#define SD SdUtils::getSd()

#if MAX_CAMERAS > 8
#error "WebServer: deadman masks hold 8 cameras"
#endif

// Staged tally map value for an explicit 0 (both 0 = clear the slot)
#define MAP_CLEAR 0xFF

#define WEB_MAX_HEADER_LINES 48   // header lines read per request
#define WEB_DRAIN_MS 1000         // discarding an unwanted request body
#define WEB_DRAIN_MAX 2048        // leftover input dropped before closing
#define WEB_SLOW_REQUEST_MS (PTZ_DEADMAN_MS / 2)   // longer: the server was busy
#define WEB_LINE_POLL_BYTES 256   // line reads: bytes per LineReader::poll()

// Deadman timers count 8 ms ticks in 16 bits
#define DM_TICKS(ms) ((uint16_t)((ms) >> 3))
static inline uint16_t dmNow() { return (uint16_t)(millis() >> 3); }

// Port-80 socket ages are 16-bit ms
static_assert(WEB_DEFER_MAX_MS < 65536UL && WEB_IDLE_SOCKET_MS < 65536UL, "socket age");

// ============================================================
// STATIC VARIABLE INITIALIZATION
// ============================================================

EthernetServer WebServer::_server(HTTP_PORT);

// SSE clients
WebServer::SseClient WebServer::_sse[MAX_SSE_CLIENTS];
uint8_t WebServer::_sseCount = 0;
unsigned long WebServer::_sseLastActivity = 0;
bool WebServer::_sseSyncWanted = false;
uint8_t WebServer::_ssePgm = 0;
uint8_t WebServer::_ssePvw = 0;
uint8_t WebServer::_sseSwitches = 0;
uint8_t WebServer::_sseStale = 0xFF;
unsigned long WebServer::_sseStateAt = 0;

// _sseStale bits and _sseSwitches bits
#define SSE_STALE_TALLY 0x01
#define SSE_STALE_STATE 0x02
#define SSE_SW_TALLY 0x01
#define SSE_SW_CCU   0x02
#define SSE_SW_VMIX  0x04

// Port-80 socket tracking
uint16_t WebServer::_sockSeenAt[MAX_SOCK_NUM];
uint8_t WebServer::_sockSeenMask = 0;

// Deadman
uint8_t WebServer::_dmPan = 0;
uint8_t WebServer::_dmZoom = 0;
uint16_t WebServer::_dmPanAt[MAX_CAMERAS];
uint16_t WebServer::_dmZoomAt[MAX_CAMERAS];
unsigned long WebServer::_dmBusyAt = 0;
uint16_t WebServer::_pollAt = 0;

// Static buffers
char WebServer::_requestBuffer[WEB_REQUEST_BUFFER_SIZE];
char WebServer::_keyBuffer[WEB_KEY_BUFFER_SIZE];
char WebServer::_valueBuffer[WEB_VALUE_BUFFER_SIZE];
char WebServer::_lineBuffer[WEB_LINE_BUFFER_SIZE];
char WebServer::_tempBuffer[WEB_TEMP_BUFFER_SIZE];
char WebServer::_sseBuffer[WEB_SSE_BUFFER_SIZE];

static const char kPanKey[] PROGMEM = "pan_tilt_velocity";
static const char kZoomKey[] PROGMEM = "set_continuous_zoom_speed";
static const char kIndexHtml[] PROGMEM = "index.html";
static const char kTallyHtml[] PROGMEM = "tally.html";
static const char kSdcardHtml[] PROGMEM = "sdcard.html";
static const char kSafemodeHtml[] PROGMEM = "safemode.html";
static const char kUploadTmp[] PROGMEM = "upload.tmp";

// Response writer over _requestBuffer (the request line is no longer needed).
// A reader that is slow to take a long answer (preset values, file list)
// is waited for with the tally, deadman and listener running.
#define RESPONSE(out, client) \
  ClientBufferPrint out(client, (uint8_t*)_requestBuffer, WEB_REQUEST_BUFFER_SIZE, \
                        WEB_RESPONSE_MS, WebServer::transferPoll)

// ============================================================
// STRICT PARSERS (no RAM literals, no truncation)
// ============================================================

// Parses "<prefix><digits><suffix>" with the suffix at the very end.
// Returns the number (saturated at LONG_MAX) or -1 if str does not match.
static long parseIndexed(const char* str, PGM_P prefix, PGM_P suffix) {
  size_t n = strlen_P(prefix);
  if (strncmp_P(str, prefix, n) != 0) return -1;
  str += n;
  if (!isdigit((unsigned char)*str)) return -1;   // no sign, space or empty number
  char* end;
  long v = strtol(str, &end, 10);
  if (strcmp_P(end, suffix) != 0) return -1;
  return v;
}

// tallyCam<N>Input -> N (isInput = true), cam<N>ID -> N (isInput = false), else -1
static long parseTallyMapKey(const char* key, bool &isInput) {
  long slot = parseIndexed(key, PSTR("tallyCam"), PSTR("Input"));
  isInput = (slot >= 0);
  if (!isInput) slot = parseIndexed(key, PSTR("cam"), PSTR("ID"));
  return slot;
}

// "a.b.c.d" with four decimal octets 0..255 and nothing else
static bool parseIPv4(const char* s, byte out[4]) {
  for (byte i = 0; i < 4; i++) {
    if (!isdigit((unsigned char)*s)) return false;
    char* end;
    long v = strtol(s, &end, 10);
    if (v > 255 || *end != (i < 3 ? '.' : '\0')) return false;
    out[i] = (byte)v;
    s = end + 1;
  }
  return true;
}

// "0" or "1" only: anything else is rejected instead of meaning false
static int8_t parseFlag(const char* s) {
  if ((s[0] == '0' || s[0] == '1') && s[1] == '\0') return s[0] - '0';
  return -1;
}

// Content-Length value: decimal digits only; -1 if missing or invalid
static long parseLength(const char* s) {
  while (*s == ' ' || *s == '\t') s++;
  if (!isdigit((unsigned char)*s)) return -1;
  char* end;
  long v = strtol(s, &end, 10);   // saturates at LONG_MAX
  while (*end == ' ' || *end == '\t') end++;
  return (*end == '\0' && v >= 0) ? v : -1;
}

// True when every comma-separated number is 0 ("0", "0,0", "0.0,-0")
static bool isZeroList(const char* s) {
  for (;;) {
    char* end;
    if (strtod(s, &end) != 0) return false;
    s = strchr(end, ',');
    if (!s) return true;
    s++;
  }
}

// Cuts s to size-1 bytes, drops an incomplete trailing UTF-8 sequence and
// replaces control characters (they would break the file and JSON)
static void clampPresetName(char* s, size_t size) {
  size_t len = strlen(s);
  if (len >= size) len = size - 1;
  size_t lead = len;
  while (lead > 0 && ((uint8_t)s[lead - 1] & 0xC0) == 0x80) lead--;
  if (lead > 0 && (uint8_t)s[lead - 1] >= 0xC0) {
    uint8_t c = s[lead - 1];
    size_t need = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
    if (len - (lead - 1) < need) len = lead - 1;
  }
  s[len] = '\0';
  for (size_t i = 0; i < len; i++) {
    if ((uint8_t)s[i] < 0x20 || s[i] == 0x7F) s[i] = ' ';
  }
}

static bool hasPrefixP(const char* s, PGM_P prefix) {
  return strncmp_P(s, prefix, strlen_P(prefix)) == 0;
}

// Request target ("/path?query HTTP/1.1", or already cut) is exactly path p
static bool pathIs(const char* target, PGM_P p) {
  size_t n = strlen_P(p);
  if (strncmp_P(target, p, n) != 0) return false;
  char c = target[n];
  return c == ' ' || c == '?' || c == '\0';
}

// The first klen bytes of q are exactly name
static bool keyIs(const char* q, size_t klen, PGM_P name) {
  return klen == strlen_P(name) && strncmp_P(q, name, klen) == 0;
}

static int8_t hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c |= 0x20;
  return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

// %XX and '+' decoded in place (HTTP-04). False on a malformed or cut
// escape, a NUL, or (allowCtl false) a control character, raw or decoded:
// the caller rejects the input instead of acting on a cut or altered one.
static bool urlDecode(char* s, bool allowCtl) {
  char* w = s;
  for (const char* r = s; *r; r++) {
    char c = *r;
    if (c == '%') {
      int8_t hi = hexval(r[1]);   // '\0' is not hex: no read past the end
      int8_t lo = (hi < 0) ? -1 : hexval(r[2]);
      if (lo < 0) return false;
      c = (char)((hi << 4) | lo);
      r += 2;
    } else if (c == '+') {
      c = ' ';
    }
    if (c == '\0' || (!allowCtl && ((uint8_t)c < 0x20 || c == 0x7F))) return false;
    *w++ = c;
  }
  *w = '\0';
  return true;
}

// Drops a %XX escape cut by a full buffer at the end of s
static void trimCutEscape(char* s) {
  size_t len = strlen(s);
  if (len >= 1 && s[len - 1] == '%') s[len - 1] = '\0';
  else if (len >= 2 && s[len - 2] == '%') s[len - 2] = '\0';
}

// SD file manager name (SEC-04, SD-06): 1..WEB_NAME_MAX printable ASCII
// without <>:"/\|?*, no leading space, no trailing dot or space. SdFat
// skips leading separators and spaces and trims trailing dots and spaces,
// so any other name could address a different file than the one checked.
// Nor a name that fits 8.3 and holds '~': SdFat also matches such a name
// against every short name, and generated ones hold '~' (INDEX~1.HTM or
// IND~5A2F.HTM is index.html). A file opened so reports its short name
// in getName(), so the alias cannot be detected after the open.
static bool validName(const char* n) {
  size_t len = strlen(n);
  if (len == 0 || len > WEB_NAME_MAX || n[0] == ' ' || n[len - 1] == '.' || n[len - 1] == ' ') {
    return false;
  }
  const char* dot = strrchr(n, '.');
  size_t base = dot ? (size_t)(dot - n) : len;
  bool shortName = base > 0 && base <= 8 && len - base <= 4;   // 8 + ".3"
  for (size_t i = 0; i < len; i++) {
    uint8_t c = (uint8_t)n[i];
    if (c < 0x20 || c >= 0x7F || strchr_P(PSTR("<>:\"/\\|?*"), c) != NULL) return false;
    if (n + i != dot && strchr_P(PSTR(" +,.;=[]"), c) != NULL) shortName = false;
  }
  return !(shortName && strchr(n, '~'));
}

// System pages and their .gz copies, in any case (FAT ignores case)
static PGM_P const kSystemFiles[] PROGMEM = {
  kIndexHtml, kTallyHtml, kSdcardHtml, kSafemodeHtml
};

static bool isSystemFile(const char* name) {
  size_t len = strlen(name);
  if (len > 3 && strcasecmp_P(name + len - 3, PSTR(".gz")) == 0) len -= 3;
  for (uint8_t i = 0; i < sizeof(kSystemFiles) / sizeof(kSystemFiles[0]); i++) {
    PGM_P f = (PGM_P)pgm_read_ptr(&kSystemFiles[i]);
    if (len == strlen_P(f) && strncasecmp_P(name, f, len) == 0) return true;
  }
  return false;
}

// Keys whose GET edits need the CSRF header: overrides, vMix IP, tally map
static bool csrfKey(const char* key) {
  if (strcmp_P(key, PSTR("overrideTally")) == 0 || strcmp_P(key, PSTR("overrideCCU")) == 0 ||
      strcmp_P(key, PSTR("vmixIP")) == 0) {
    return true;
  }
  bool isInput;
  return parseTallyMapKey(key, isInput) >= 0;
}

// Strict JSON number: -?(0|[1-9]d*)(.d+)?([eE][+-]?d+)?
static bool isJsonNumber(const char* s, size_t n) {
  size_t i = 0;
  if (i < n && s[i] == '-') i++;
  if (i >= n || !isdigit((unsigned char)s[i])) return false;
  if (s[i] == '0') i++;
  else while (i < n && isdigit((unsigned char)s[i])) i++;
  if (i < n && s[i] == '.') {
    if (++i >= n || !isdigit((unsigned char)s[i])) return false;
    while (i < n && isdigit((unsigned char)s[i])) i++;
  }
  if (i < n && (s[i] == 'e' || s[i] == 'E')) {
    if (++i < n && (s[i] == '+' || s[i] == '-')) i++;
    if (i >= n || !isdigit((unsigned char)s[i])) return false;
    while (i < n && isdigit((unsigned char)s[i])) i++;
  }
  return i == n;
}

// One preset value: a JSON number as is, empty as 0 (CCUControl applies an
// empty list field as 0), anything else as a string (SD-04)
static void printPresetValue(Print &out, const char* s, size_t n) {
  if (n == 0) {
    out.print('0');
  } else if (isJsonNumber(s, n)) {
    out.write((const uint8_t*)s, n);
  } else {
    out.print('"');
    WebServer::printJsonSafe(out, s, n);
    out.print('"');
  }
}

// ============================================================
// BOUNDED I/O HELPERS
// ============================================================

// Request body budget: base plus Content-Length at WEB_BODY_BYTES_PER_MS
static unsigned long bodyDeadline(unsigned long length) {
  unsigned long ms = WEB_BODY_MS + length / WEB_BODY_BYTES_PER_MS;
  if (ms > WEB_BODY_MAX_MS) ms = WEB_BODY_MAX_MS;
  return NetUtils::deadlineIn(ms);
}

// Discards the rest of a request body that is still arriving, so the
// close is not a reset (bounded in bytes and time; the tally keeps running).
// Callers skip it for a body that stopped (idle, deadline, disconnect).
static void drainBody(EthernetClient &client, long remaining) {
  unsigned long deadline = NetUtils::deadlineIn(WEB_DRAIN_MS);
  while (remaining > 0) {
    remaining -= NetUtils::drainInput(client, remaining, 0);
    if (remaining <= 0 || NetUtils::expired(deadline) ||
        (client.available() == 0 && !client.connected())) {
      break;
    }
    WebServer::transferPoll();
  }
}

static uint8_t sockCount() {
#if MAX_SOCK_NUM > 4
  if (W5100.getChip() == 51) return 4;   // the W5100 has 4 sockets
#endif
  return MAX_SOCK_NUM;
}

// b[0..n) starts with pat: 1; is a proper prefix of it: 0 (undecided); else -1
static int8_t startsWithP(const char* b, size_t n, PGM_P pat) {
  size_t m = strlen_P(pat);
  if (n < m) return strncmp_P(b, pat, n) == 0 ? 0 : -1;
  return strncmp_P(b, pat, m) == 0 ? 1 : -1;
}

static const char kPathEvents[] PROGMEM = "events";
static const char kPathReboot[] PROGMEM = "reboot";
static const char kPathStatus[] PROGMEM = "safemode-status";
static PGM_P const kNoFilePaths[] PROGMEM = { kPathEvents, kPathReboot, kPathStatus };

// From the first bytes of a request not read yet: true if it may get a
// file (page, static file, /?download=) or is too short to tell
static bool wantsTransfer(EthernetClient &c) {
  char b[24];
  size_t n = NetUtils::peek(c, (uint8_t*)b, sizeof(b));
  int8_t m = startsWithP(b, n, PSTR("GET /"));
  if (m <= 0) return m == 0;
  const char* t = b + 5;
  n -= 5;
  if (n > 0 && t[0] == '?') return startsWithP(t, n, PSTR("?download=")) >= 0;
  for (uint8_t i = 0; i < sizeof(kNoFilePaths) / sizeof(kNoFilePaths[0]); i++) {
    PGM_P p = (PGM_P)pgm_read_ptr(&kNoFilePaths[i]);
    m = startsWithP(t, n, p);
    if (m == 0) return true;
    size_t k = strlen_P(p);
    if (m > 0) return n == k || (t[k] != ' ' && t[k] != '?');
  }
  return true;
}

// First occurrence of pat in buf, or -1
static int findPattern(const uint8_t* buf, size_t len, const char* pat, size_t plen) {
  if (plen == 0 || len < plen) return -1;
  for (size_t i = 0; i + plen <= len; i++) {
    if (buf[i] == (uint8_t)pat[0] && memcmp(buf + i, pat, plen) == 0) return (int)i;
  }
  return -1;
}

// Content-Length framed body read in blocks, with a total deadline and an
// idle limit; the tally keeps running while it waits
class BodyReader {
 public:
  BodyReader(EthernetClient &c, long length, unsigned long deadline, uint8_t* buf, uint8_t size)
    : _c(c), _remaining(length), _deadline(deadline), _idleAt(millis()),
      _buf(buf), _size(size), _pos(0), _len(0) {}

  // Next body byte, -1 at the end of the body, on deadline, disconnect or
  // WEB_BODY_IDLE_MS without data
  int read() {
    if (_pos < _len) return _buf[_pos++];
    while (_remaining > 0) {
      wdt_reset();
      WebServer::transferPoll();
      int avail = _c.available();
      if (avail > 0) {
        int n = avail < _size ? avail : _size;
        if (n > _remaining) n = (int)_remaining;
        n = _c.read(_buf, n);
        if (n > 0) {
          _remaining -= n;
          _len = (uint8_t)n;
          _pos = 1;
          _idleAt = millis();
          return _buf[0];
        }
      } else if (!_c.connected() || millis() - _idleAt >= WEB_BODY_IDLE_MS) {
        break;
      }
      if (NetUtils::expired(_deadline)) break;
    }
    return -1;
  }
  long left() const { return _remaining + (_len - _pos); }   // not yet returned
  long unread() const { return _remaining; }                 // still in the socket

 private:
  EthernetClient &_c;
  long _remaining;
  unsigned long _deadline;
  unsigned long _idleAt;
  uint8_t* _buf;
  uint8_t _size, _pos, _len;
};

// ============================================================
// JSON ESCAPING
// ============================================================

void WebServer::printJsonSafe(Print &out, const char* str) {
  if (str) printJsonSafe(out, str, strlen(str));
}

// No switch: it could become a lookup table in RAM
void WebServer::printJsonSafe(Print &out, const char* str, size_t len) {
  for (size_t i = 0; i < len; i++) {
    uint8_t c = (uint8_t)str[i];
    if (c == '"' || c == '\\') {
      out.print('\\');
      out.print((char)c);
    } else if (c >= 0x20) {
      out.print((char)c);
    } else if (c == '\n') {
      out.print(F("\\n"));
    } else if (c == '\r') {
      out.print(F("\\r"));
    } else if (c == '\t') {
      out.print(F("\\t"));
    } else {
      uint8_t d = c & 0x0F;
      out.print(F("\\u00"));
      out.print((char)((c >> 4) ? '1' : '0'));
      out.print((char)(d < 10 ? '0' + d : 'a' + d - 10));
    }
  }
}

// ============================================================
// INITIALIZATION
// ============================================================

bool WebServer::begin() {
  _server.begin();
  Serial.print(F("Web server on port "));
  Serial.println(HTTP_PORT);
  return true;
}

// ============================================================
// PROCESS REQUESTS - MAIN ENTRY POINT
// ============================================================

void WebServer::processRequests() {
  wdt_reset();

  // SSE clients first: their sockets are skipped by the scan below
  processSSEClients();

  // File transfers in the background: a slice per pass, so requests on
  // other sockets are answered meanwhile (also skipped by the scan)
  SdUtils::pumpTransfers();

  // Oldest waiting request first, a few per pass (HTTP-01). A long request
  // or requests left waiting make the server busy for the deadman: a
  // joystick refresh may be among them or still on its way.
  unsigned long start = millis();
  for (uint8_t n = 0;; n++) {
    uint8_t sock = scanSockets();
    if (sock >= MAX_SOCK_NUM) break;
    if (n >= WEB_MAX_PER_PASS || millis() - start >= WEB_PASS_MS) {
      _dmBusyAt = millis();
      break;
    }
    unsigned long t0 = millis();
    serveClient(sock);
    wdt_reset();
    if (millis() - t0 >= WEB_SLOW_REQUEST_MS) _dmBusyAt = millis();
    VmixConnector::pollTally();
  }

  checkDeadman(false);
}

void WebServer::transferPoll() {
  uint16_t now = (uint16_t)millis();
  if ((uint16_t)(now - _pollAt) < WEB_POLL_MS) return;
  _pollAt = now;
  VmixConnector::pollTally();
  checkDeadman(true);
  keepListening();
}

// Sweeps the port-80 sockets and returns the one whose request has waited
// longest (MAX_SOCK_NUM if none). The library's available() serves the
// highest index first, keeps no listener between a SYN and the next call,
// and never closes a connection that sends nothing.
uint8_t WebServer::scanSockets() {
  uint8_t count = sockCount();
  uint16_t now = (uint16_t)millis();
  uint8_t best = MAX_SOCK_NUM;
  uint16_t bestAge = 0;
  uint8_t listening = 0, freeSocks = 0;

  // With every transfer slot busy a page or file request waits in its
  // socket (0: no wait). Oldest first, so it gets the next slot that frees,
  // within WEB_DEFER_MAX_MS; only WEB_DEFER_MS while sockets are short
  // (vMix must be able to reconnect). Then it is served: a 503. So is one
  // whose client has closed its side (a reload): it gave up.
  uint16_t deferMs = 0;
  if (!SdUtils::transferSlotFree()) {
    uint8_t closed = 0;
    for (uint8_t i = 0; i < count; i++) {
      if (EthernetClient(i).status() == SnSR::CLOSED) closed++;
    }
    deferMs = (closed >= WEB_DEFER_SPARE) ? WEB_DEFER_MAX_MS : WEB_DEFER_MS;
  }

  for (uint8_t i = 0; i < count; i++) {
    uint8_t bit = (uint8_t)(1 << i);
    EthernetClient c(i);
    uint8_t sr = c.status();
    if (sr == SnSR::CLOSED) freeSocks++;

    // SSE streams and file transfers are served elsewhere
    bool http = (EthernetServer::server_port[i] == HTTP_PORT) && !SdUtils::transferOwns(i);
    for (uint8_t k = 0; k < _sseCount; k++) {
      if (_sse[k].sock == i) http = false;
    }
    if (http && sr == SnSR::LISTEN) listening++;
    if (!http || (sr != SnSR::ESTABLISHED && sr != SnSR::CLOSE_WAIT)) {
      _sockSeenMask &= (uint8_t)~bit;
      continue;
    }

    if (!(_sockSeenMask & bit)) {
      _sockSeenMask |= bit;
      _sockSeenAt[i] = now;
    }
    uint16_t age = (uint16_t)(now - _sockSeenAt[i]);
    if (c.available() > 0) {
      if (sr == SnSR::ESTABLISHED && age < deferMs && wantsTransfer(c)) continue;
      if (best == MAX_SOCK_NUM || age > bestAge) {
        best = i;
        bestAge = age;
      }
    } else if (sr == SnSR::CLOSE_WAIT || age >= WEB_IDLE_SOCKET_MS) {
      // Closed before sending anything, or connected and silent (SEC-02).
      // FIN, so a browser drops a preconnected socket instead of using it.
      NetUtils::closeGracefully(c, 0);
      _sockSeenMask &= (uint8_t)~bit;
      freeSocks++;
    }
  }

  // Always one listener; a second one only while sockets are plentiful
  // (vMix reconnects and the 8098 listener need a free socket)
  if (listening == 0 || (listening < WEB_LISTENERS && freeSocks >= 3)) {
    _server.begin();
  }
  return best;
}

// During long transfers: re-arm the port-80 listener taken by a new
// connection. Only from a CLOSED socket (begin() would otherwise close one
// that is closing); vMix and the 8098 listener cannot use one meanwhile.
void WebServer::keepListening() {
  uint8_t count = sockCount();
  uint8_t freeSocks = 0;
  for (uint8_t i = 0; i < count; i++) {
    uint8_t sr = EthernetClient(i).status();
    if (sr == SnSR::CLOSED) freeSocks++;
    else if (sr == SnSR::LISTEN && EthernetServer::server_port[i] == HTTP_PORT) return;
  }
  if (freeSocks >= 1) _server.begin();
}

void WebServer::serveClient(uint8_t sock) {
  EthernetClient client(sock);
  _sockSeenMask &= (uint8_t)~(1 << sock);

  // If the client vanishes mid-request, transferPoll() may re-arm a
  // listener on its index and a new client land there: the final drain
  // and close only touch the connection served here.
  NetUtils::Peer peer;
  NetUtils::peerOf(client, &peer);

  // Request line and headers share one total budget
  unsigned long deadline = NetUtils::deadlineIn(WEB_HEADER_MS);
  NetUtils::LineReader lr(_requestBuffer, WEB_REQUEST_BUFFER_SIZE);
  int n = readLinePolled(client, lr, deadline, NULL);
  if (n == 0) {
    // one stray CRLF before the request line is allowed
    n = readLinePolled(client, lr, deadline, NULL);
  }
  if (n <= 0) {
    if (NetUtils::samePeer(client, peer)) NetUtils::closeGracefully(client, 0);   // FIN, no wait
    return;
  }
  bool tooLong = lr.truncated();

  Request req;
  readHeaders(client, deadline, req);
  wdt_reset();

  if (tooLong) {
    // Never act on a cut request line (HTTP-09)
    drainBody(client, req.contentLength);
    sendBareStatus(client, 414);
  } else if (hasPrefixP(_requestBuffer, PSTR("GET "))) {
    if (!pathIs(_requestBuffer + 4, PSTR("/events"))) {
      processGETRequest(_requestBuffer, client, req);
    } else if (req.crossSite) {
      sendForbidden(client);   // another site must not take an SSE slot (SEC-06)
    } else if (handleSSEConnection(client)) {
      return;  // kept open
    }
  } else if (hasPrefixP(_requestBuffer, PSTR("POST "))) {
    processPOSTRequest(_requestBuffer, client, req);
  } else {
    sendBareStatus(client, 501);   // OPTIONS too: no CORS preflight passes
  }

  // Unread input would turn the close into a reset. A client slow to close
  // its side is waited for with the tally, deadman and listener running.
  // (A file transfer has released client: nothing to do then.)
  if (!NetUtils::samePeer(client, peer)) return;
  NetUtils::drainInput(client, WEB_DRAIN_MAX, 0);
  NetUtils::closeGracefully(client, WEB_CLOSE_MS, transferPoll);
}

// Header lines up to the empty line (or the deadline). Keeps what the
// handlers need; the multipart end pattern goes to _valueBuffer.
void WebServer::readHeaders(EthernetClient &client, unsigned long deadline, Request &req) {
  req.contentLength = -1;
  req.gzip = false;
  req.boundary = false;
  req.csrf = false;
  req.crossSite = false;

  NetUtils::LineReader lr(_lineBuffer, WEB_LINE_BUFFER_SIZE);
  for (uint8_t lines = 0; lines < WEB_MAX_HEADER_LINES; lines++) {
    int n = readLinePolled(client, lr, deadline, NULL);
    bool cut = lr.truncated();
    if (n == 0) return;
    if (n < 0) {
      req.contentLength = -1;   // headers incomplete: no body framing
      return;
    }
    char* line = _lineBuffer;
    if (strncasecmp_P(line, PSTR("Content-Length:"), 15) == 0) {
      req.contentLength = cut ? -1 : parseLength(line + 15);
    } else if (strncasecmp_P(line, PSTR("Accept-Encoding:"), 16) == 0) {
      req.gzip = strcasestr_P(line + 16, PSTR("gzip")) != NULL;
    } else if (strncasecmp_P(line, PSTR("X-TallyCCU:"), 11) == 0) {
      // A browser sends a custom header cross-origin only after a CORS
      // preflight, which never passes here (SEC-01)
      const char* v = line + 11;
      v += strspn_P(v, PSTR(" \t"));
      req.csrf = (v[0] == '1' && v[1 + strspn_P(v + 1, PSTR(" \t"))] == '\0');
    } else if (strncasecmp_P(line, PSTR("Sec-Fetch-Site:"), 15) == 0) {
      const char* v = line + 15;
      v += strspn_P(v, PSTR(" \t"));
      req.crossSite = strncasecmp_P(v, PSTR("same-origin"), 11) != 0 &&
                      strncasecmp_P(v, PSTR("none"), 4) != 0;
    } else if (strncasecmp_P(line, PSTR("Content-Type:"), 13) == 0) {
      // Only multipart/form-data carries the upload boundary (HTTP-11)
      const char* type = line + 13;
      type += strspn_P(type, PSTR(" \t"));
      req.boundary = false;
      char* b = (strncasecmp_P(type, PSTR("multipart/form-data"), 19) == 0)
                ? strcasestr_P(line + 13, PSTR("boundary=")) : NULL;
      if (b) {
        b += 9;
        if (*b == '"') b++;
        size_t len = strcspn_P(b, PSTR("\"; \t"));
        // A cut line leaves a prefix of the boundary: still fine if long
        bool partial = cut && b[len] == '\0';
        if (len > 0 && len <= WEB_VALUE_BUFFER_SIZE - 5 && (!partial || len >= 16)) {
          strcpy_P(_valueBuffer, PSTR("\r\n--"));
          memcpy(_valueBuffer + 4, b, len);
          _valueBuffer[4 + len] = '\0';
          req.boundary = true;
        }
      }
    }
  }
}

// One line (NetUtils::LineReader rules: '\r' dropped, a long line is cut
// and lr.truncated() set). remaining (optional) bounds a Content-Length body.
// -1 at the end of the body, on deadline, disconnect or WEB_BODY_IDLE_MS
// without data; a partial line is then lost (close the connection).
int WebServer::readLinePolled(EthernetClient &client, NetUtils::LineReader &lr,
                              unsigned long deadline, long* remaining) {
  unsigned long idleAt = millis();
  for (;;) {
    wdt_reset();
    if (client.available() > 0) idleAt = millis();
    int n = lr.poll(client, WEB_LINE_POLL_BYTES, remaining);
    if (n >= 0) return n;
    if (remaining && *remaining <= 0) return -1;
    if ((client.available() == 0 && !client.connected()) ||
        millis() - idleAt >= WEB_BODY_IDLE_MS || NetUtils::expired(deadline)) {
      return -1;
    }
    transferPoll();
  }
}

// ============================================================
// PROCESS POST - FOR SAVING PRESETS
// ============================================================

void WebServer::processPOSTRequest(const char* reqLine, EthernetClient &client, const Request &req) {
#if DEBUG_HOT
  Serial.println(F("[POST] Received"));
#endif

  const char* target = reqLine + 5;

  // Companion: no CSRF header
  if (pathIs(target, PSTR("/savePreset"))) {
    handleSavePresetPOST(client, req.contentLength);
    return;
  }

  bool exitSafe = pathIs(target, PSTR("/safemode-exit"));
  bool sync = pathIs(target, PSTR("/syncState"));
  bool upload = pathIs(target, PSTR("/upload"));
  if (!exitSafe && !sync && !upload) {
    drainBody(client, req.contentLength);
    sendBareStatus(client, 404);
    return;
  }

  // The rest needs the CSRF header (SEC-01)
  if (!req.csrf) {
    drainBody(client, req.contentLength);
    sendForbidden(client);
    return;
  }

  if (exitSafe) {
    drainBody(client, req.contentLength);
    if (!SafeMode::isActive()) {
      // Outside Safe Mode it would only be a restart
      sendJSONResponse(client, 404, false, F("Not in safe mode"));
      return;
    }
    handleSafeModeExit(client);
  } else if (sync) {
    // Sync state from web to TCP clients
    handleSyncStatePOST(client, req.contentLength);
  } else {
    handleUploadFile(client, req);
  }
}

// ============================================================
// SAVE PRESET VIA POST - STREAMING WITH URL DECODE
// ============================================================

void WebServer::handleSavePresetPOST(EthernetClient &client, long contentLength) {
#if DEBUG_HOT
  Serial.println(F("[POST] savePreset"));
  Serial.print(F("Content-Length: "));
  Serial.println(contentLength);
#endif

  if (contentLength <= 0) {
    sendJSONResponse(client, 400, false, F("No content"));
    return;
  }
  if (contentLength > WEB_PRESET_MAX_BODY) {
    drainBody(client, contentLength);
    sendJSONResponse(client, 400, false, F("Too large"));
    return;
  }

  // The body is read through _requestBuffer; the response comes after it
  BodyReader in(client, contentLength, bodyDeadline(contentLength),
                (uint8_t*)_requestBuffer, WEB_REQUEST_BUFFER_SIZE);

  // Body "cameraId=X&presetId=Y&name=Z&data=...": parse field by field
  // up to data= (no fixed prefix limit; a long name is cut, not rejected)
  long cameraId = -1;
  long presetId = -1;
  char presetName[MAX_PRESET_NAME_LENGTH];
  strcpy_P(presetName, PSTR("Preset"));

  int keyLen = 0;
  int valLen = 0;
  bool inValue = false;
  bool valOverflow = false;
  bool foundData = false;
  bool badField = false;

  for (;;) {
    int ch = in.read();
    bool end = (ch < 0);
    char c = (char)ch;

    if (!inValue) {
      if (end) break;
      if (c == '=') {
        _keyBuffer[keyLen] = '\0';
        if (strcmp_P(_keyBuffer, PSTR("data")) == 0) {
          foundData = true;
          break;
        }
        inValue = true;
        valLen = 0;
        valOverflow = false;
      } else if (c == '&') {
        keyLen = 0;
      } else if (keyLen < WEB_KEY_BUFFER_SIZE - 1) {
        _keyBuffer[keyLen++] = c;
      }
      continue;
    }

    if (!end && c != '&') {
      if (valLen < WEB_VALUE_BUFFER_SIZE - 1) _valueBuffer[valLen++] = c;
      else valOverflow = true;
      continue;
    }

    // End of a field value
    _valueBuffer[valLen] = '\0';
    if (strcmp_P(_keyBuffer, PSTR("cameraId")) == 0) {
      cameraId = valOverflow ? -1 : parseIndexed(_valueBuffer, PSTR(""), PSTR(""));
    } else if (strcmp_P(_keyBuffer, PSTR("presetId")) == 0) {
      presetId = valOverflow ? -1 : parseIndexed(_valueBuffer, PSTR(""), PSTR(""));
    } else if (strcmp_P(_keyBuffer, PSTR("name")) == 0) {
      // 95 encoded bytes decode to >= 31 bytes: overflow only loses the
      // tail (an escape cut there is dropped). Control characters become
      // spaces in clampPresetName(); a malformed escape fails the save.
      if (valOverflow) trimCutEscape(_valueBuffer);
      if (urlDecode(_valueBuffer, true)) {
        clampPresetName(_valueBuffer, sizeof(presetName));
        strcpy(presetName, _valueBuffer);
      } else {
        badField = true;
      }
    }
    inValue = false;
    keyLen = 0;
    if (end) break;
  }

#if DEBUG_HOT
  Serial.print(F("Cam:")); Serial.print(cameraId);
  Serial.print(F(" P:")); Serial.print(presetId);
  Serial.print(F(" N:")); Serial.println(presetName);
#endif

  if (!foundData || badField || cameraId < 1 || cameraId > MAX_CAMERAS ||
      presetId < 0 || presetId >= MAX_PRESETS) {
    if (foundData) drainBody(client, in.unread());   // else it ended or stopped
    sendJSONResponse(client, 400, false, F("Invalid params"));
    return;
  }

  if (!SdUtils::isAvailable()) {
    drainBody(client, in.unread());
    sendJSONResponse(client, 500, false, F("SD not available"));
    return;
  }

  // Written to preset_X_Y.tmp; it replaces the preset only once complete
  char* tmpName = _tempBuffer;
  SdUtils::getPresetFilename(tmpName, WEB_TEMP_BUFFER_SIZE, cameraId, presetId, true);
  SdUtils::removeFile(tmpName);   // leftover of an interrupted save

  File presetFile = SD.open(tmpName, FILE_WRITE);
  if (!presetFile) {
    drainBody(client, in.unread());
    sendJSONResponse(client, 500, false, F("File error"));
    return;
  }

  // Metadata
  presetFile.print(F("cameraId:"));
  presetFile.println(cameraId);
  presetFile.print(F("presetId:"));
  presetFile.println(presetId);
  presetFile.print(F("name:"));
  presetFile.println(presetName);

  // Read data with real-time URL decode
  int paramCount = 0;
  int paramLen = 0;
  bool paramOverflow = false;
  bool cutEscape = false;
  bool badEscape = false;
  int skipped = 0;   // over-long params: dropped whole, never cut

  for (;;) {
    int ch = in.read();
    if (ch < 0) break;
    char c = (char)ch;

    // URL decode (HTTP-04). in.read() waits (bounded) for digits in a later
    // segment. A malformed escape or a control character (a decoded '\n'
    // would add a line to the file) fails the save.
    bool raw = true;
    if (c == '%') {
      int h1 = in.read();
      int h2 = (h1 < 0) ? -1 : in.read();
      if (h2 < 0) {
        if (in.left() == 0) badEscape = true;   // '%' at the end of the body
        else cutEscape = true;                  // the body stopped
        break;
      }
      int8_t hi = hexval((char)h1);
      int8_t lo = hexval((char)h2);
      if (hi < 0 || lo < 0) {
        badEscape = true;
        break;
      }
      c = (char)((hi << 4) | lo);
      raw = false;
    } else if (c == '+') {
      c = ' ';
    }
    if (raw && (c == '\r' || c == '\n')) {
      c = ';';   // a raw line break (curl, a file) ends the parameter
    } else if ((uint8_t)c < 0x20 || c == 0x7F) {
      badEscape = true;
      break;
    }

    // Process decoded character
    if (c == ';') {
      _lineBuffer[paramLen] = '\0';
      if (paramOverflow) {
        skipped++;
      } else if (paramLen > 0) {
        presetFile.println(_lineBuffer);
        paramCount++;
      }
      paramLen = 0;
      paramOverflow = false;
    } else if (paramLen < WEB_LINE_BUFFER_SIZE - 1) {
      _lineBuffer[paramLen++] = c;
    } else {
      paramOverflow = true;
    }
  }

  bool complete = !cutEscape && !badEscape && in.left() == 0;

  // Last parameter
  if (complete) {
    if (paramOverflow) {
      skipped++;
    } else if (paramLen > 0) {
      _lineBuffer[paramLen] = '\0';
      presetFile.println(_lineBuffer);
      paramCount++;
    }
  }

  bool writeOk = !presetFile.getWriteError();
  writeOk = presetFile.close() && writeOk;   // close() syncs to the card

  bool committed = writeOk && complete;
  if (committed) {
    char* finalName = _keyBuffer;
    SdUtils::getPresetFilename(finalName, WEB_KEY_BUFFER_SIZE, cameraId, presetId, false);
    writeOk = SdUtils::replaceFile(tmpName, finalName);
  }

  if (!writeOk || !complete) {
    // A partial preset would load only some values: the old one is kept.
    // Only a bad escape leaves body to drain (else the body stopped).
    if (!committed) SdUtils::removeFile(tmpName);   // else replaceFile() handled it
    Serial.print(F("Preset save failed: "));
    Serial.println(tmpName);
    if (badEscape) {
      drainBody(client, in.unread());
      sendJSONResponse(client, 400, false, F("Bad encoding"));
    } else if (!writeOk) {
      sendJSONResponse(client, 500, false, F("Write error"));
    } else {
      sendJSONResponse(client, 400, false, F("Incomplete data"));
    }
    return;
  }

#if DEBUG_HOT
  Serial.print(F("OK: "));
  Serial.print(paramCount);
  Serial.println(F(" params"));
#endif

  // Notify TCP clients that preset was saved
  CCUBroadcast::sendPresetSaved(cameraId, presetId, presetName);

  // Notify SSE client (web)
  sendSSEPresetSaved(cameraId, presetId, presetName);

  if (skipped > 0) {
    RESPONSE(out, client);
    sendJSONHead(out, 200, true);
    out.print(F("OK, "));
    out.print(skipped);
    out.print(F(" skipped (too long)"));
    sendJSONTail(out, paramCount);
    out.flush();
  } else {
    sendJSONResponse(client, 200, true, F("OK"), paramCount);
  }
}

// ============================================================
// SYNC STATE FROM WEB TO TCP CLIENTS
// ============================================================

void WebServer::handleSyncStatePOST(EthernetClient &client, long contentLength) {
#if DEBUG_HOT
  Serial.println(F("[POST] syncState"));
#endif

  if (contentLength <= 0) {
    sendJSONResponse(client, 200, false, F("No content"));
    return;
  }
  if (contentLength > WEB_SYNC_MAX_BODY) {
    drainBody(client, contentLength);
    sendJSONResponse(client, 400, false, F("Too large"));
    return;
  }

  // Body "cam:key:value" lines. A line split across segments is joined;
  // one that does not fit is dropped whole, never forwarded cut (HTTP-05).
  int paramCount = 0;
  long remaining = contentLength;
  unsigned long deadline = bodyDeadline(contentLength);
  NetUtils::LineReader lr(_lineBuffer, WEB_LINE_BUFFER_SIZE);

  int dropped = 0;
  while (remaining > 0) {
    transferPoll();   // a long body that keeps flowing
    int len = readLinePolled(client, lr, deadline, &remaining);
    if (len < 0) break;   // deadline, idle or disconnect
    if (lr.truncated()) {
      dropped++;   // counted in the answer (HTTP-09)
      continue;
    }
    if (len == 0) continue;

    // Parse "cam:key:value"
    char* firstColon = strchr(_lineBuffer, ':');
    if (!firstColon) continue;

    *firstColon = '\0';
    long cameraId = parseIndexed(_lineBuffer, PSTR(""), PSTR(""));   // atoi wraps at 16 bits

    char* secondColon = strchr(firstColon + 1, ':');
    if (!secondColon) continue;

    *secondColon = '\0';
    const char* paramKey = firstColon + 1;
    const char* value = secondColon + 1;

    if (cameraId >= 1 && cameraId <= MAX_CAMERAS && strlen(paramKey) > 0) {
      // Broadcast to TCP clients (Companion)
      CCUBroadcast::sendParamChange((int)cameraId, paramKey, value);
      paramCount++;
    }
  }

#if DEBUG_HOT
  Serial.print(F("[syncState] Forwarded "));
  Serial.print(paramCount);
  Serial.println(F(" params to TCP"));
#endif

  if (dropped > 0) {
    RESPONSE(out, client);
    sendJSONHead(out, 200, true);
    out.print(F("OK, "));
    out.print(dropped);
    out.print(F(" dropped (too long)"));
    sendJSONTail(out, paramCount);
    out.flush();
  } else {
    sendJSONResponse(client, 200, true, F("OK"), paramCount);
  }
}

// ============================================================
// SEND RESPONSES
// ============================================================

// "HTTP/1.1 <code> <reason>\r\n"
static void printStatusLine(Print &out, int httpCode) {
  out.print(F("HTTP/1.1 "));
  out.print(httpCode);
  switch (httpCode) {
    case 200: out.print(F(" OK\r\n")); break;
    case 400: out.print(F(" Bad Request\r\n")); break;
    case 403: out.print(F(" Forbidden\r\n")); break;
    case 404: out.print(F(" Not Found\r\n")); break;
    case 414: out.print(F(" URI Too Long\r\n")); break;
    case 501: out.print(F(" Not Implemented\r\n")); break;
    case 503: out.print(F(" Service Unavailable\r\n")); break;
    default:  out.print(F(" Internal Server Error\r\n")); break;
  }
}

// Status line and headers. No Access-Control-Allow-Origin: the pages are
// same-origin and Companion's fetch does not apply CORS (SEC-06).
void WebServer::sendHeaders(Print &out, int httpCode, bool json) {
  printStatusLine(out, httpCode);
  out.print(json ? F("Content-Type: application/json\r\n") : F("Content-Type: text/plain\r\n"));
  out.print(F("Connection: close\r\n\r\n"));
}

// Status line only (404 unknown POST, 414 cut request line, 501 other methods)
void WebServer::sendBareStatus(EthernetClient &client, int httpCode) {
  RESPONSE(out, client);
  printStatusLine(out, httpCode);
  out.print(F("Connection: close\r\n\r\n"));
  out.flush();
}

// Missing CSRF header (SEC-01)
void WebServer::sendForbidden(EthernetClient &client) {
#if DEBUG_HOT
  Serial.println(F("[HTTP] 403: no X-TallyCCU header"));
#endif
  RESPONSE(out, client);
  sendHeaders(out, 403, false);
  out.print(F("ERROR forbidden\r\n"));
  out.flush();
}

// File manager answer: {"success":true} or {"success":false,"error":"<error>"}
void WebServer::sendFileResult(EthernetClient &client, int httpCode, PGM_P error) {
  RESPONSE(out, client);
  sendHeaders(out, httpCode, true);
  if (error) {
    out.print(F("{\"success\":false,\"error\":\""));
    out.print((const __FlashStringHelper*)error);
    out.print(F("\"}\r\n"));
  } else {
    out.print(F("{\"success\":true}\r\n"));
  }
  out.flush();
}

// Plain-text answer to a GET with parameters: "OK" or "ERROR <reason>"
// (messages indexed by ParamResult, in flash: a switch here builds RAM tables)
static const char kMsgOk[] PROGMEM = "OK";
static const char kMsgKey[] PROGMEM = "ERROR unknown key";
static const char kMsgLength[] PROGMEM = "ERROR key or value too long";
static const char kMsgCamera[] PROGMEM = "ERROR bad cameraId";
static const char kMsgSlot[] PROGMEM = "ERROR bad tally slot";
static const char kMsgInput[] PROGMEM = "ERROR bad vMix input";
static const char kMsgMapCamera[] PROGMEM = "ERROR bad tally camera";
static const char kMsgValue[] PROGMEM = "ERROR bad value";
static const char kMsgSafeMode[] PROGMEM = "ERROR safe mode";
static const char* const kParamMessages[] PROGMEM = {
  kMsgOk, kMsgKey, kMsgLength, kMsgCamera, kMsgSlot, kMsgInput, kMsgMapCamera, kMsgValue, kMsgSafeMode
};

void WebServer::sendParamResult(EthernetClient &client, ParamResult result) {
  int code = (result == PARAM_OK) ? 200 : (result == PARAM_ERR_SAFE_MODE) ? 503 : 400;
  if (result > PARAM_ERR_SAFE_MODE) result = PARAM_ERR_KEY;
  RESPONSE(out, client);
  sendHeaders(out, code, false);
  out.print((const __FlashStringHelper*)pgm_read_ptr(&kParamMessages[result]));
  out.print(F("\r\n"));
  out.flush();
}

// {"success":bool,"message":str[,"paramCount":n]}
void WebServer::sendJSONResponse(EthernetClient &client, int httpCode, bool success, const __FlashStringHelper* message, int paramCount) {
  RESPONSE(out, client);
  sendJSONHead(out, httpCode, success);
  out.print(message);
  sendJSONTail(out, paramCount);
  out.flush();
}

// message must not point into _requestBuffer (it is the output buffer)
void WebServer::sendJSONResponse(EthernetClient &client, int httpCode, bool success, const char* message, int paramCount) {
  RESPONSE(out, client);
  sendJSONHead(out, httpCode, success);
  printJsonSafe(out, message);
  sendJSONTail(out, paramCount);
  out.flush();
}

void WebServer::sendJSONHead(Print &out, int httpCode, bool success) {
  sendHeaders(out, httpCode, true);
  out.print(success ? F("{\"success\":true,\"message\":\"") : F("{\"success\":false,\"message\":\""));
}

void WebServer::sendJSONTail(Print &out, int paramCount) {
  out.print('"');
  if (paramCount >= 0) {
    out.print(F(",\"paramCount\":"));
    out.print(paramCount);
  }
  out.print(F("}\r\n"));
}

// 200 {"<key>":true|false}
void WebServer::sendJSONBool(EthernetClient &client, PGM_P key, bool value) {
  RESPONSE(out, client);
  sendHeaders(out, 200, true);
  out.print(F("{\""));
  out.print((const __FlashStringHelper*)key);
  out.print(F("\":"));
  out.print(value ? F("true") : F("false"));
  out.print(F("}\r\n"));
  out.flush();
}

// SD page or file: SdUtils answers (404/503 included) and, on success, sends
// the body in the background with the socket (client is then released)
void WebServer::serveSdFile(EthernetClient &client, PGM_P filename, bool gzip) {
  strcpy_P(_tempBuffer, filename);
  SdUtils::serveFile(client, _tempBuffer, gzip);
}

// safemode.html, or the built-in page when it cannot be read: no SD card,
// card failing or the file missing (BOOT-13). A busy 503 stays (retry).
void WebServer::serveSafeModePage(EthernetClient &client, bool gzip) {
  strcpy_P(_tempBuffer, kSafemodeHtml);
  bool unreadable;
  if (SdUtils::serveFile(client, _tempBuffer, gzip, false, &unreadable) || !unreadable) return;

  Serial.println(F("safemode.html not readable: built-in page"));
  RESPONSE(out, client);
  out.print(F("HTTP/1.1 200 OK\r\n"
              "Content-Type: text/html; charset=UTF-8\r\n"
              "Cache-Control: no-store\r\n"
              "Connection: close\r\n\r\n"));
  SafeMode::printFallbackPage(out);
  out.flush();
}

// ============================================================
// PROCESS GET - MAIN FUNCTIONALITY
// ============================================================

void WebServer::processGETRequest(char* reqLine, EthernetClient &client, const Request &req) {
  // "GET <path>[?<query>] HTTP/1.1": cut in place (serveClient made sure
  // the line was not truncated). Paths match exactly (HTTP-12).
  char* path = reqLine + 4;
  char* end = strchr(path, ' ');
  if (end) *end = '\0';
  char* query = strchr(path, '?');
  if (query) *query++ = '\0';
  if (path[0] != '/') {
    send404(client);
    return;
  }
  bool root = (path[1] == '\0');

  // ============================================================
  // SAFE MODE HANDLING - CHECK FIRST
  // ============================================================

  // Safe mode status API (always available)
  if (strcmp_P(path, PSTR("/safemode-status")) == 0) {
    handleSafeModeStatus(client);
    return;
  }

  bool mainPage = (root && !query) || strcmp_P(path, PSTR("/index.html")) == 0;

  // In safe mode, redirect main pages to diagnostic page
  if (SafeMode::isActive() && (mainPage || strcmp_P(path, PSTR("/ccu")) == 0)) {
    serveSafeModePage(client, req.gzip);
    return;
  }

  // ============================================================
  // NORMAL REQUEST HANDLING
  // ============================================================

  // Reboot (CSRF header required, SEC-01)
  if (strcmp_P(path, PSTR("/reboot")) == 0) {
    if (!req.csrf) {
      sendForbidden(client);
      return;
    }
    Serial.println(F("Reboot requested"));
    {
      RESPONSE(out, client);
      sendHeaders(out, 200, false);
      out.print(F("Rebooting...\r\n"));
      out.flush();
    }
    NetUtils::closeGracefully(client, WEB_CLOSE_MS);
    SafeMode::restart();   // intentional: not counted as a boot failure
    return;
  }

  // Pages
  if (mainPage) {
    serveSdFile(client, kIndexHtml, req.gzip);
    return;
  }
  if (strcmp_P(path, PSTR("/tally.html")) == 0) {
    serveSdFile(client, kTallyHtml, req.gzip);
    return;
  }
  if (strcmp_P(path, PSTR("/sd")) == 0 || strcmp_P(path, PSTR("/sd/")) == 0) {
    serveSdFile(client, kSdcardHtml, req.gzip);
    return;
  }

  // Static files: decoded path. serveFile() answers, its 404/503 included
  // (body in the background).
  if (!root) {
    char* name = path + 1;
    if (!urlDecode(name, false) || strlen(name) >= WEB_TEMP_BUFFER_SIZE) {
      send404(client);
      return;
    }
    strcpy(_tempBuffer, name);
    SdUtils::serveFile(client, _tempBuffer, req.gzip);
    return;
  }

  processQuery(query, client, req);
}

// "/?<query>": endpoint by its first key, else parameters
void WebServer::processQuery(char* query, EthernetClient &client, const Request &req) {
  // First key, and its value cut at the next raw '&' (an encoded one stays
  // in it); next: the rest of the query, if any
  size_t klen = strcspn_P(query, PSTR("=&"));
  char* next = NULL;
  char* raw = NULL;
  if (query[klen] == '=') {
    raw = query + klen + 1;
    char* amp = strchr(raw, '&');
    if (amp) {
      *amp = '\0';
      next = amp + 1;
    }
  }

  if (keyIs(query, klen, PSTR("getOverrides"))) {
    handleGetOverrides(client);
    return;
  }
  if (keyIs(query, klen, PSTR("getTallyMap"))) {
    handleGetTallyMap(client);
    return;
  }
  if (keyIs(query, klen, PSTR("getVmixConnect"))) {
    handleGetVmixConnect(client);
    return;
  }

  // getParams - Legacy endpoint (SSE is now primary sync method)
  // Returns empty - web uses SSE for real-time state
  if (keyIs(query, klen, PSTR("getParams"))) {
    long cameraId = raw ? parseIndexed(raw, PSTR(""), PSTR("")) : -1;
    if (cameraId < 1 || cameraId > MAX_CAMERAS) {
      cameraId = CCUControl::getActiveCamera();
    }
    RESPONSE(out, client);
    sendHeaders(out, 200, true);
    out.print(F("{\"cameraId\":"));
    out.print(cameraId);
    out.print(F(",\"paramCount\":0,\"params\":{}}\r\n"));
    out.flush();
    return;
  }

  if (keyIs(query, klen, PSTR("listPresets"))) {
#if DEBUG_HOT
    Serial.println(F("Listing presets"));
#endif
    listPresetsAsJSON(client);
    return;
  }

  // loadPreset=cam,idx[&returnValues=0|1] (Companion: no CSRF header)
  if (keyIs(query, klen, PSTR("loadPreset"))) {
    if (next) next[-1] = '&';   // handleLoadPreset reads the options
    handleLoadPreset(client, raw ? raw : query + klen);
    return;
  }

  // ============================================================
  // SD FILE MANAGEMENT
  // ============================================================

  bool isList = keyIs(query, klen, PSTR("listFiles"));
  bool isDownload = keyIs(query, klen, PSTR("download"));
  bool isExists = keyIs(query, klen, PSTR("fileExists"));
  bool isDelete = keyIs(query, klen, PSTR("deleteFile"));
  bool isRename = keyIs(query, klen, PSTR("renameFile"));

  if (isList) {
    handleListFiles(client);
    return;
  }

  if (isDownload || isExists || isDelete || isRename) {
    if ((isDelete || isRename) && !req.csrf) {
      sendForbidden(client);
      return;
    }
    // Decoded in place and checked (SEC-04, SD-06); NULL if not acceptable
    char* name = raw;
    if (name && !(urlDecode(name, false) && validName(name))) name = NULL;

    if (isRename) {
      // renameFile=<old>&to=<new>, each decoded on its own: an encoded
      // '&' in a name no longer splits it (SD-05)
      char* newName = NULL;
      if (next && strncmp_P(next, PSTR("to="), 3) == 0) {
        newName = next + 3;
        char* amp = strchr(newName, '&');
        if (amp) *amp = '\0';
        if (!(urlDecode(newName, false) && validName(newName))) newName = NULL;
      }
      if (!name || !newName) {
        sendFileResult(client, 400, PSTR("bad name"));
        return;
      }
      handleRenameFile(client, name, newName);   // names stay in the request line
      return;
    }

    if (!name) {
      if (isExists) sendJSONResponse(client, 400, false, F("Bad file name"));
      else if (isDelete) sendFileResult(client, 400, PSTR("bad name"));
      else send404(client);
      return;
    }
    // The response is written over the request line
    strcpy(_tempBuffer, name);
    if (isDownload) handleDownloadFile(client, _tempBuffer);
    else if (isExists) handleFileExists(client, _tempBuffer);
    else handleDeleteFile(client, _tempBuffer);
    return;
  }

  // ============================================================
  // PARAMETERS
  // ============================================================

  // Restore the first '&' (the first value was cut for the endpoints above)
  if (next) next[-1] = '&';

  // Overrides, vMix IP and tally map need the CSRF header (SEC-01): checked
  // for the whole query first, so nothing of it is applied without it
  if (!req.csrf) {
    for (const char* p = query; *p;) {
      size_t n = strcspn_P(p, PSTR("=&"));
      if (p[n] == '=' && n > 0 && n < WEB_KEY_BUFFER_SIZE) {
        memcpy(_keyBuffer, p, n);
        _keyBuffer[n] = '\0';
        if (csrfKey(_keyBuffer)) {
          sendForbidden(client);
          return;
        }
      }
      p = strchr(p, '&');
      if (!p) break;
      p++;
    }
  }

  // Every valid pair is applied; the first error decides the answer
  // (200 OK, 400 or 503 "ERROR <reason>").
  ParamContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ParamResult result = PARAM_OK;

  for (char* p = query; *p;) {
    char* amp = strchr(p, '&');
    if (amp) *amp = '\0';

    if (*p) {   // skip empty pairs ("&&", trailing '&')
      char* eq = strchr(p, '=');
      ParamResult r;
      if (!eq || eq == p) {
        r = PARAM_ERR_KEY;      // empty key or no '='
      } else if (eq - p >= WEB_KEY_BUFFER_SIZE || strlen(eq + 1) >= WEB_VALUE_BUFFER_SIZE) {
        r = PARAM_ERR_LENGTH;   // never act on a truncated key or value
      } else {
        *eq = '\0';
        strcpy(_keyBuffer, p);
        strcpy(_valueBuffer, eq + 1);
        r = handleParam(_keyBuffer, _valueBuffer, ctx);
      }
      if (r != PARAM_OK) {
        if (r != PARAM_ERR_SAFE_MODE) ctx.badRequest = true;
        if (result == PARAM_OK) result = r;
      }
    }

    if (!amp) break;
    p = amp + 1;
  }

  // A slot is cleared only with both values 0 (TAL-07); a lone 0 is a 400
  for (byte s = 0; s < MAXTALLIES; s++) {
    bool inClear = (ctx.mapInput[s] == MAP_CLEAR);
    bool camClear = (ctx.mapCam[s] == MAP_CLEAR);
    if (inClear != camClear) {
      ctx.badRequest = true;
      if (result == PARAM_OK) result = inClear ? PARAM_ERR_INPUT : PARAM_ERR_MAP_CAMERA;
    }
  }

  // Tally map edits are all-or-nothing: after a 400 the map is untouched
  if (!ctx.badRequest) {
    for (byte s = 0; s < MAXTALLIES; s++) {
      if (ctx.mapInput[s] == MAP_CLEAR) {
        TallyManager::clearMapping(s);
      } else if (ctx.mapInput[s] || ctx.mapCam[s]) {
        TallyManager::setMapping(s, ctx.mapInput[s], ctx.mapCam[s]);
      }
    }
  }

  // Deadman only with deadman=1 and a non-zero speed; anything else
  // (Companion, an explicit stop) disarms it
  if (ctx.pan) armDeadman(false, ctx.panCam, ctx.deadman && ctx.pan == 2);
  if (ctx.zoom) armDeadman(true, ctx.zoomCam, ctx.deadman && ctx.zoom == 2);

  sendParamResult(client, result);
}

// ============================================================
// LOAD PRESET
// ============================================================

// args: "cam,idx" then optional "&returnValues=0|1"; other keys ignored
void WebServer::handleLoadPreset(EthernetClient &client, char* args) {
  bool returnValues = false;
  bool bad = false;
  char* amp = strchr(args, '&');
  if (amp) {
    *amp = '\0';
    for (char* p = amp + 1; p;) {
      char* next = strchr(p, '&');
      if (next) *next++ = '\0';
      if (strncmp_P(p, PSTR("returnValues="), 13) == 0) {
        int8_t f = parseFlag(p + 13);   // only 0/1: "=0" is no longer a yes
        if (f < 0) bad = true;
        else returnValues = (f == 1);
      }
      p = next;
    }
  }

  // "cam,idx", both strict decimal
  long cameraId = -1;
  long presetId = -1;
  char* comma = strchr(args, ',');
  if (comma) {
    *comma = '\0';
    cameraId = parseIndexed(args, PSTR(""), PSTR(""));
    presetId = parseIndexed(comma + 1, PSTR(""), PSTR(""));
  }

  if (bad || cameraId < 1 || cameraId > MAX_CAMERAS || presetId < 0 || presetId >= MAX_PRESETS) {
    sendJSONResponse(client, 400, false, F("Invalid params"));
    return;
  }

  if (!CCUControl::isReady()) {
    sendJSONResponse(client, 503, false, F("safe mode"));
    return;
  }

  // Without a card every preset would look missing
  if (!SdUtils::isAvailable()) {
    sendJSONResponse(client, 500, false, F("SD not available"));
    return;
  }

  SdUtils::getPresetFilename(_tempBuffer, WEB_TEMP_BUFFER_SIZE, cameraId, presetId);
  if (!SdUtils::fileExists(_tempBuffer)) {
    // A card pulled since the check above fails here first
    if (!SdUtils::isAvailable()) {
      sendJSONResponse(client, 500, false, F("SD not available"));
    } else {
      sendJSONResponse(client, 404, false, F("Preset not found"));
    }
    return;
  }

#if DEBUG_HOT
  Serial.print(F("Loading preset cam:"));
  Serial.print(cameraId);
  Serial.print(F(" preset:"));
  Serial.println(presetId);
#endif

  if (!SdUtils::applyPreset(cameraId, presetId)) {
    sendJSONResponse(client, 500, false, F("Preset load error"));
    return;
  }

  if (returnValues) {
    sendJSONPresetValues(client, cameraId, presetId);
  } else {
    sendJSONResponse(client, 200, true, F("OK"));
  }
}

// ============================================================
// PRESET VALUES AS JSON
// ============================================================

// {"success":true,"parameters":{...},"name":"..."} (SD-04): the name is no
// longer a parameter; numbers that are valid JSON go as numbers, anything
// else as an escaped string; a list keeps empty fields (as 0). Values are
// stored decoded: not decoded again (SD-05). Only called after a
// successful applyPreset.
void WebServer::sendJSONPresetValues(EthernetClient &client, int cameraId, int presetId) {
  SdUtils::getPresetFilename(_tempBuffer, WEB_TEMP_BUFFER_SIZE, cameraId, presetId);
  File presetFile = SD.open(_tempBuffer);

  RESPONSE(out, client);
  sendHeaders(out, 200, true);
  out.print(F("{\"success\":true,\"parameters\":{"));

  char name[MAX_PRESET_NAME_LENGTH];
  strcpy_P(name, PSTR("Preset"));
  bool firstParam = true;

  while (presetFile && !out.failed()) {
    wdt_reset();
    bool cut;
    int len = SdUtils::readLine(presetFile, _lineBuffer, WEB_LINE_BUFFER_SIZE, &cut);
    if (len < 0) break;
    if (cut || len == 0) continue;   // a cut value could read as another number

    char* colon = strchr(_lineBuffer, ':');
    if (!colon || colon == _lineBuffer) continue;
    *colon = '\0';
    const char* key = _lineBuffer;
    const char* val = colon + 1;

    if (strcmp_P(key, PSTR("cameraId")) == 0 || strcmp_P(key, PSTR("presetId")) == 0) {
      continue;
    }
    if (strcmp_P(key, PSTR("name")) == 0) {
      strncpy(name, val, sizeof(name) - 1);
      name[sizeof(name) - 1] = '\0';
      continue;
    }

    if (!firstParam) out.print(',');
    firstParam = false;
    out.print('"');
    printJsonSafe(out, key);
    out.print(F("\":"));

    if (strchr(val, ',') != NULL) {
      out.print('[');
      for (;;) {
        const char* comma = strchr(val, ',');
        size_t n = comma ? (size_t)(comma - val) : strlen(val);
        printPresetValue(out, val, n);
        if (!comma) break;
        out.print(',');
        val = comma + 1;
      }
      out.print(']');
    } else {
      printPresetValue(out, val, strlen(val));
    }
  }

  out.print(F("},\"name\":\""));
  printJsonSafe(out, name);
  out.print(F("\"}\r\n"));
  out.flush();
  presetFile.close();
}

// ============================================================
// HANDLE GET PARAMETERS
// ============================================================

WebServer::ParamResult WebServer::handleParam(const char* key, char* value, ParamContext &ctx) {
  wdt_reset();
  if (!urlDecode(value, false)) return PARAM_ERR_VALUE;   // HTTP-04

  // vmixConnect
  if (strcmp_P(key, PSTR("vmixConnect")) == 0) {
    int8_t on = parseFlag(value);
    if (on < 0) return PARAM_ERR_VALUE;
    VmixConnector::setConnectEnabled(on == 1);
    return PARAM_OK;
  }

  // vmixIP
  if (strcmp_P(key, PSTR("vmixIP")) == 0) {
    byte ip[4];
    if (!parseIPv4(value, ip)) return PARAM_ERR_VALUE;
    snprintf_P(_tempBuffer, WEB_TEMP_BUFFER_SIZE, PSTR("%u.%u.%u.%u"), ip[0], ip[1], ip[2], ip[3]);
    changeVMixIP(_tempBuffer);
    return PARAM_OK;
  }

  // cameraId: target of the CCU params that follow
  if (strcmp_P(key, PSTR("cameraId")) == 0) {
    long cam = parseIndexed(value, PSTR(""), PSTR(""));
    if (cam < 1 || cam > MAX_CAMERAS) {
      ctx.camera = -1;   // following CCU params must not go to another camera
      return PARAM_ERR_CAMERA;
    }
    ctx.camera = (int8_t)cam;
    CCUControl::setActiveCamera(cam);
    return PARAM_OK;
  }

  // deadman=1: the PTZ / zoom speed of this request stops by itself unless
  // refreshed within PTZ_DEADMAN_MS (web joystick only)
  if (strcmp_P(key, PSTR("deadman")) == 0) {
    int8_t on = parseFlag(value);
    if (on < 0) return PARAM_ERR_VALUE;
    ctx.deadman = (on == 1);
    return PARAM_OK;
  }

  // Overrides
  if (strcmp_P(key, PSTR("overrideTally")) == 0) {
    int8_t on = parseFlag(value);
    if (on < 0) return PARAM_ERR_VALUE;
    TallyManager::setOverride(on == 1);
    return PARAM_OK;
  }

  if (strcmp_P(key, PSTR("overrideCCU")) == 0) {
    if (!CCUControl::isReady()) return PARAM_ERR_SAFE_MODE;
    int8_t on = parseFlag(value);
    if (on < 0) return PARAM_ERR_VALUE;
    CCUControl::setOverride(on == 1);
    return PARAM_OK;
  }

  // Tally map: tallyCam<slot>Input=<vMix input>, cam<slot>ID=<camera>,
  // both 0 to clear the slot. Staged in ctx; processGETRequest commits it
  // if the query had no 400.
  bool isInput;
  long slot = parseTallyMapKey(key, isInput);
  if (slot >= 0) {
    if (slot >= MAXTALLIES) return PARAM_ERR_SLOT;
    long v = parseIndexed(value, PSTR(""), PSTR(""));
    if (isInput) {
      if (v == 0) v = MAP_CLEAR;
      else if (v < 1 || v > MAX_VMIX_INPUTS) return PARAM_ERR_INPUT;
      ctx.mapInput[slot] = (byte)v;
    } else {
      if (v == 0) v = MAP_CLEAR;
      else if (v < 1 || v > MAX_CAMERAS) return PARAM_ERR_MAP_CAMERA;
      ctx.mapCam[slot] = (byte)v;
    }
    return PARAM_OK;
  }

  // Anything else is a CCU parameter
  if (ctx.camera < 0) return PARAM_ERR_CAMERA;
  if (!CCUControl::isReady()) {
    return CCUControl::findParamDefByKey(key) ? PARAM_ERR_SAFE_MODE : PARAM_ERR_KEY;
  }
  if (CCUControl::applyParameterByKey(key, value, ctx.camera > 0 ? ctx.camera : -1)) {
    // Remember motion commands for the deadman
    bool zoom = (strcmp_P(key, kZoomKey) == 0);
    if (zoom || strcmp_P(key, kPanKey) == 0) {
      uint8_t state = isZeroList(value) ? 1 : 2;
      uint8_t cam = (uint8_t)(ctx.camera > 0 ? ctx.camera : CCUControl::getActiveCamera());
      if (zoom) {
        ctx.zoom = state;
        ctx.zoomCam = cam;
      } else {
        ctx.pan = state;
        ctx.panCam = cam;
      }
    }
    return PARAM_OK;
  }
  return CCUControl::findParamDefByKey(key) ? PARAM_ERR_VALUE : PARAM_ERR_KEY;
}

// ============================================================
// PTZ / ZOOM DEADMAN (HTTP-01)
// ============================================================

void WebServer::armDeadman(bool zoom, uint8_t cam, bool arm) {
  if (cam < 1 || cam > MAX_CAMERAS) return;
  uint8_t bit = (uint8_t)(1 << (cam - 1));
  uint8_t &mask = zoom ? _dmZoom : _dmPan;
  if (arm) {
    mask |= bit;
    (zoom ? _dmZoomAt : _dmPanAt)[cam - 1] = dmNow();
  } else {
    mask &= (uint8_t)~bit;
  }
}

// Sends the stop for every armed speed not refreshed within PTZ_DEADMAN_MS,
// or PTZ_DEADMAN_BUSY_MS while (or right after) the server is busy
void WebServer::checkDeadman(bool busy) {
  if (!(_dmPan | _dmZoom)) return;
  if (millis() - _dmBusyAt < PTZ_DEADMAN_MS) busy = true;
  uint16_t limit = busy ? DM_TICKS(PTZ_DEADMAN_BUSY_MS) : DM_TICKS(PTZ_DEADMAN_MS);
  uint16_t now = dmNow();
  for (uint8_t i = 0; i < MAX_CAMERAS; i++) {
    uint8_t bit = (uint8_t)(1 << i);
    for (uint8_t z = 0; z < 2; z++) {
      uint8_t &mask = z ? _dmZoom : _dmPan;
      uint16_t at = z ? _dmZoomAt[i] : _dmPanAt[i];
      if (!(mask & bit) || (uint16_t)(now - at) < limit) continue;
      mask &= (uint8_t)~bit;

      char key[sizeof(kZoomKey)];
      char val[4];
      strcpy_P(key, z ? kZoomKey : kPanKey);
      strcpy_P(val, z ? PSTR("0") : PSTR("0,0"));
      Serial.print(F("[PTZ] Deadman stop, cam "));
      Serial.println(i + 1);
      CCUControl::applyParameterByKey(key, val, i + 1);
    }
  }
}

// ============================================================
// FUNCIONES HTTP AUXILIARES
// ============================================================

void WebServer::handleGetOverrides(EthernetClient &client) {
  RESPONSE(out, client);
  sendHeaders(out, 200, true);
  out.print(F("{\"overrideTally\":"));
  out.print(TallyManager::getOverride() ? F("true") : F("false"));
  out.print(F(",\"overrideCCU\":"));
  out.print(CCUControl::getOverride() ? F("true") : F("false"));
  out.print(F(",\"ccuAvailable\":"));
  out.print(CCUControl::isReady() ? F("true") : F("false"));
  out.print(F("}\r\n"));
  out.flush();
}

void WebServer::handleGetTallyMap(EthernetClient &client) {
  RESPONSE(out, client);
  sendHeaders(out, 200, true);
  out.print(F("{\"mappings\":["));
  byte inp, cam;
  for (byte i = 0; i < MAXTALLIES; i++) {
    TallyManager::getMapping(i, inp, cam);
    if (i > 0) out.print(',');
    out.print(F("{\"input\":"));
    out.print(inp);
    out.print(F(",\"camId\":"));
    out.print(cam);
    out.print('}');
  }
  out.print(F("]}\r\n"));
  out.flush();
}

void WebServer::handleGetVmixConnect(EthernetClient &client) {
  byte vmixIP[4];
  VmixConnector::getVmixIP(vmixIP);

  RESPONSE(out, client);
  sendHeaders(out, 200, true);
  out.print(F("{\"vmixConnectEnabled\":"));
  out.print(VmixConnector::getConnectEnabled() ? F("true") : F("false"));
  out.print(F(",\"vmixConnected\":"));
  out.print(VmixConnector::isConnected() ? F("true") : F("false"));
  out.print(F(",\"vmixIP\":\""));
  for (byte i = 0; i < 4; i++) {
    if (i > 0) out.print('.');
    out.print(vmixIP[i]);
  }
  out.print(F("\"}\r\n"));
  out.flush();
}

void WebServer::listPresetsAsJSON(EthernetClient &client) {
  {
    RESPONSE(out, client);
    sendHeaders(out, 200, true);
    out.flush();
    if (out.failed()) return;
  }
  // NetUtils block writes: a SEND still in flight is waited for there
  SdUtils::writePresetListToClient(client);
}

void WebServer::send404(EthernetClient &client) {
  RESPONSE(out, client);
  sendHeaders(out, 404, false);
  out.print(F("404 Not Found\r\n"));
  out.flush();
}

// ============================================================
// SD FILE MANAGEMENT
// ============================================================

// Root files, system pages and their .gz hidden (any case). A name longer
// than WEB_NAME_MAX comes as "" (SdFat), one with non-ASCII characters
// with '?': neither can be addressed. "error":"sd" without a card.
void WebServer::handleListFiles(EthernetClient &client) {
  bool sdOk = SdUtils::isAvailable();   // may remount (~2 s): before the response budget

  RESPONSE(out, client);
  sendHeaders(out, 200, true);
  out.print(F("{\"files\":["));

  char rootPath[2] = { '/', '\0' };
  File root;
  if (sdOk) root = SD.open(rootPath);
  if (root) {
    bool first = true;
    File entry;
    char fileName[WEB_NAME_MAX + 1];
    while (!out.failed() && (entry = root.openNextFile())) {
      if (!entry.isDirectory()) {
        entry.getName(fileName, sizeof(fileName));

        if (!isSystemFile(fileName)) {
          if (!first) out.print(',');
          first = false;

          out.print(F("{\"name\":\""));
          printJsonSafe(out, fileName);
          out.print(F("\",\"size\":"));
          out.print((unsigned long)entry.size());
          out.print('}');
        }
      }
      entry.close();
      wdt_reset();
      transferPoll();   // a long directory walk
    }
    root.close();
  }

  out.print(']');
  if (!sdOk) out.print(F(",\"error\":\"sd\""));
  out.print(F("}\r\n"));
  out.flush();
}

// A background transfer like the pages (404 for a missing file or a
// directory). validName(): no '"' or control characters in the header.
void WebServer::handleDownloadFile(EthernetClient &client, const char* filename) {
  if (!SdUtils::isAvailable()) {
    sendJSONResponse(client, 503, false, F("SD not available"));
    return;
  }
  SdUtils::serveFile(client, filename, false, true);
}

void WebServer::handleFileExists(EthernetClient &client, const char* filename) {
  if (!SdUtils::isAvailable()) {
    sendJSONResponse(client, 503, false, F("SD not available"));
    return;
  }
  sendJSONBool(client, PSTR("exists"), SdUtils::fileExists(filename));
}

// System pages and their .gz cannot be deleted (SEC-04)
void WebServer::handleDeleteFile(EthernetClient &client, const char* filename) {
  if (isSystemFile(filename)) {
    sendFileResult(client, 400, PSTR("protected"));
    return;
  }
  if (!SdUtils::isAvailable()) {
    sendFileResult(client, 503, PSTR("sd"));
    return;
  }
  if (!SdUtils::fileExists(filename)) {
    bool sd = SdUtils::isAvailable();
    sendFileResult(client, sd ? 404 : 503, sd ? PSTR("not found") : PSTR("sd"));
    return;
  }
  if (!SdUtils::removeFile(filename)) {
    sendFileResult(client, 500, PSTR("sd"));
    return;
  }
  SdUtils::invalidateGzip(filename);
  sendFileResult(client, 200, NULL);
}

// Neither name may be a system page (SEC-04). The names live in the
// request line: rename first, then answer.
void WebServer::handleRenameFile(EthernetClient &client, const char* oldName, const char* newName) {
  PGM_P error = NULL;
  int code = 400;
  if (isSystemFile(oldName) || isSystemFile(newName)) {
    error = PSTR("protected");
  } else if (!SdUtils::isAvailable()) {
    error = PSTR("sd");
    code = 503;
  } else if (!SdUtils::fileExists(oldName)) {
    error = PSTR("not found");
    code = 404;
  } else if (SdUtils::fileExists(newName)) {
    error = PSTR("exists");   // also the same name in another case
  } else if (!SdUtils::renameFile(oldName, newName)) {
    error = PSTR("sd");
    code = 500;
  }
  if (!error) {
    SdUtils::invalidateGzip(oldName);
    SdUtils::invalidateGzip(newName);
    code = 200;
  }
  sendFileResult(client, code, error);
}

// Multipart upload, streamed to upload.tmp and renamed over the target
// only when the closing boundary arrived and every write worked (SD-02).
// Errors answer 200 with success:false (the page shows the message).
// The caller checked the CSRF header: with it a system page may be
// replaced (how the UI is updated from sdcard.html, SEC-04).
void WebServer::handleUploadFile(EthernetClient &client, const Request &req) {
#if DEBUG_HOT
  Serial.println(F("[POST] upload"));
#endif

  long remaining = req.contentLength;
  if (remaining <= 0 || !req.boundary) {
    drainBody(client, remaining);
    sendJSONResponse(client, 200, false, F("Bad request"));
    return;
  }
  if (!SdUtils::isAvailable()) {
    drainBody(client, remaining);
    sendJSONResponse(client, 200, false, F("SD not available"));
    return;
  }

  unsigned long deadline = bodyDeadline(remaining);
  const char* pattern = _valueBuffer;   // "\r\n--<boundary>" from readHeaders
  size_t patLen = strlen(pattern);
  char* filename = _tempBuffer;
  filename[0] = '\0';

  // Part headers line by line (HTTP-05): the file starts after the empty
  // line ending the headers of a part with filename="...". A name that is
  // cut (long line or longer than WEB_NAME_MAX) is rejected, never shortened.
  // Read through _requestBuffer (the request line is done with; the content
  // loop reuses it after): Content-Disposition with a WEB_NAME_MAX name fits.
  char* hline = _requestBuffer;
  NetUtils::LineReader lr(hline, WEB_REQUEST_BUFFER_SIZE);
  int8_t name = 0;   // this part: 0 no file, 1 filename, -1 bad filename
  bool inContent = false;
  bool stopped = false;   // the body stopped: nothing to drain
  while (remaining > 0) {
    int n = readLinePolled(client, lr, deadline, &remaining);
    if (n < 0) {
      stopped = true;
      break;
    }
    if (n == 0) {
      if (name == 0) continue;   // a plain field: its value follows
      inContent = (name > 0);
      break;
    }
    if (hline[0] == '-' && hline[1] == '-') {
      name = 0;                  // boundary: a new part
      continue;
    }
    char* fnPtr = strstr_P(hline, PSTR("filename=\""));
    if (fnPtr) {
      fnPtr += 10;
      uint8_t i = 0;
      while (fnPtr[i] && fnPtr[i] != '"' && i < WEB_TEMP_BUFFER_SIZE - 1) {
        filename[i] = fnPtr[i];
        i++;
      }
      filename[i] = '\0';
      name = (fnPtr[i] != '"') ? -1 : (i > 0) ? 1 : 0;
    } else if (lr.truncated() &&
               strncasecmp_P(hline, PSTR("Content-Disposition:"), 20) == 0) {
      name = -1;                 // filename= may be past the cut
    }
  }

  if (!inContent) {
    if (!stopped) drainBody(client, remaining);
    sendJSONResponse(client, 200, false, name < 0 ? F("Bad filename") :
                     name > 0 ? F("Incomplete data") : F("No file"));
    return;
  }

  // The name must be the file it addresses (validName) and not the temp file
  char* tmpName = _keyBuffer;
  strcpy_P(tmpName, kUploadTmp);
  if (!validName(filename) || strcasecmp(filename, tmpName) == 0) {
    drainBody(client, remaining);
    sendJSONResponse(client, 200, false, F("Bad filename"));
    return;
  }

  SdUtils::removeFile(tmpName);   // leftover of an interrupted upload
  File uploadFile = SD.open(tmpName, FILE_WRITE);
  if (!uploadFile) {
    drainBody(client, remaining);
    sendJSONResponse(client, 200, false, F("SD error"));
    return;
  }

  // Content in blocks (WDT-02). The last patLen-1 bytes stay in the buffer
  // in case the end pattern is split across reads.
  uint8_t* buf = (uint8_t*)_requestBuffer;
  size_t keep = patLen - 1;
  size_t len = 0;
  bool found = false;
  bool writeOk = true;
  unsigned long lastData = millis();

  while (remaining > 0 && writeOk) {
    wdt_reset();
    transferPoll();
    int avail = client.available();
    int r = 0;
    if (avail > 0) {
      size_t want = WEB_REQUEST_BUFFER_SIZE - len;
      if ((long)want > remaining) want = (size_t)remaining;
      if ((size_t)avail < want) want = (size_t)avail;
      r = client.read(buf + len, want);
    }
    if (r <= 0) {
      if (!client.connected() || NetUtils::expired(deadline) ||
          millis() - lastData >= WEB_BODY_IDLE_MS) {
        stopped = true;
        break;
      }
      continue;
    }
    remaining -= r;
    len += (size_t)r;
    lastData = millis();

    int pos = findPattern(buf, len, pattern, patLen);
    size_t out = (pos >= 0) ? (size_t)pos : (len > keep ? len - keep : 0);
    if (out > 0 && uploadFile.write(buf, out) != out) writeOk = false;
    if (pos >= 0) {
      found = true;
      break;
    }
    memmove(buf, buf + out, len - out);
    len -= out;
  }

  writeOk = !uploadFile.getWriteError() && writeOk;
  writeOk = uploadFile.close() && writeOk;
  bool committed = found && writeOk;
  bool ok = committed;
  if (committed) {
    ok = SdUtils::replaceFile(tmpName, filename);
    writeOk = ok;
    if (ok) SdUtils::invalidateGzip(filename);   // or the old .gz is served
  }
  if (!ok) {
    if (!committed) SdUtils::removeFile(tmpName);   // else replaceFile() handled it
    Serial.print(F("Upload failed: "));
    Serial.println(filename);
  }

  // Closing "--" of the last boundary and any epilogue
  if (!stopped) drainBody(client, remaining);

  if (ok) {
    sendJSONResponse(client, 200, true, filename);
  } else {
    sendJSONResponse(client, 200, false, writeOk ? F("Incomplete data") : F("Write error"));
  }
}

// ============================================================
// SAFE MODE HANDLERS
// ============================================================

void WebServer::handleSafeModeStatus(EthernetClient &client) {
  // Calculate free RAM
  extern int __heap_start, *__brkval;
  int v;
  int freeRam = (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);

  RESPONSE(out, client);
  out.print(F("HTTP/1.1 200 OK\r\n"
              "Content-Type: application/json\r\n"
              "Cache-Control: no-cache\r\n"
              "Connection: close\r\n\r\n"));

  // Build JSON response
  out.print(F("{\"safeMode\":"));
  out.print(SafeMode::isActive() ? F("true") : F("false"));
  out.print(F(",\"resetCount\":"));
  out.print(SafeMode::getResetCount());
  out.print(F(",\"resetReason\":\""));
  out.print(SafeMode::getResetReasonString());
  out.print(F("\",\"uptime\":"));
  out.print(SafeMode::getUptime());
  out.print(F(",\"freeRam\":"));
  out.print(freeRam);
  out.print(F(",\"ip\":\""));
  out.print(NetworkManager::getLocalIP());
  out.print(F("\",\"firmware\":\"" FIRMWARE_VERSION "\"}\r\n"));
  out.flush();
}

void WebServer::handleSafeModeExit(EthernetClient &client) {
  // Send response first
  {
    RESPONSE(out, client);
    sendHeaders(out, 200, true);
    out.print(F("{\"success\":true,\"message\":\"Restarting...\"}\r\n"));
    out.flush();
  }

  // Close connection (bounded), then exit safe mode (this triggers a restart)
  NetUtils::closeGracefully(client, WEB_CLOSE_MS);
  SafeMode::exitSafeMode();
}

// ============================================================
// SSE (SERVER-SENT EVENTS) HANDLING
// ============================================================

// Up to MAX_SSE_CLIENTS; a new one replaces the oldest (HTTP-03).
// Returns false if the stream could not be started (caller closes).
bool WebServer::handleSSEConnection(EthernetClient &client) {
  {
    RESPONSE(out, client);
    out.print(F("HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: keep-alive\r\n\r\n"
                "event: connected\r\n"
                "data: {\"status\":\"ok\"}\r\n\r\n"));
    out.flush();
    if (out.failed()) return false;
  }

  // Drop stale slots first: a full table must only evict a live client
  for (uint8_t i = _sseCount; i-- > 0;) sseValid(i);
  if (_sseCount >= MAX_SSE_CLIENTS) {
    Serial.println(F("[SSE] Replacing oldest client"));
    EthernetClient old(_sse[0].sock);
    NetUtils::closeGracefully(old, 50);
    sseRemove(0);
  }

  SseClient &s = _sse[_sseCount++];
  s.sock = client.getSocketNumber();
  s.drops = 0;
  s.rport = client.remotePort();
  s.rip = (uint32_t)client.remoteIP();
  _sseLastActivity = millis();

  Serial.print(F("[SSE] New client connected, clients: "));
  Serial.println(_sseCount);

  // Tally and switches on the next pass (to every client: harmless)
  _sseStale = SSE_STALE_TALLY | SSE_STALE_STATE;

  // REQUESTSYNC to Companion; CCUBroadcast rate limits it (CCU_SYNC_INTERVAL)
  // and sends a deferred one while an SSE client is still connected
  CCUBroadcast::requestSync();
  return true;
}

// True if SSE slot i still holds its connection. A socket index reused by
// someone else is forgotten without touching it; a peer that closed is
// closed and forgotten.
bool WebServer::sseValid(uint8_t i) {
  SseClient &s = _sse[i];
  EthernetClient c(s.sock);
  uint8_t sr = c.status();
  if (sr == SnSR::ESTABLISHED || sr == SnSR::CLOSE_WAIT) {
    bool same = c.localPort() == HTTP_PORT && c.remotePort() == s.rport &&
                (uint32_t)c.remoteIP() == s.rip;
    if (same && sr == SnSR::ESTABLISHED) return true;
    if (same) {
      Serial.println(F("[SSE] Client disconnected"));
      NetUtils::closeGracefully(c, 0);   // the peer closed: answer its FIN
      sseRemove(i);
      return false;
    }
  }
  Serial.println(F("[SSE] Client lost"));
  sseRemove(i);
  return false;
}

void WebServer::sseRemove(uint8_t i) {
  if (i >= _sseCount) return;
  _sseCount--;
  memmove(&_sse[i], &_sse[i + 1], (_sseCount - i) * sizeof(SseClient));
}

// Sends one composed event to every SSE client, all or nothing per client.
// An event that does not fit is dropped and counted; a client that keeps
// dropping (SSE_MAX_DROPS in a row or SSE_STALL_MS) is closed (WDT-01).
void WebServer::sseBroadcast(const ClientBufferPrint &event) {
  if (event.failed()) return;   // longer than WEB_SSE_BUFFER_SIZE
  for (uint8_t i = _sseCount; i-- > 0;) {
    if (!sseValid(i)) continue;
    SseClient &s = _sse[i];
    EthernetClient c(s.sock);
    bool dead = false;
    bool ok;
    unsigned long t0 = millis();
    for (;;) {
      ok = NetUtils::tryWriteAll(c, event.data(), event.length(), &dead);
      if (ok || dead || s.drops || millis() - t0 >= SSE_TX_WAIT_MS) break;
      wdt_reset();
    }
    if (ok) {
      if (s.drops) sseResync(c, s);
      continue;
    }
    if (dead) {
      Serial.println(F("[SSE] Client gone"));
      sseRemove(i);
      continue;
    }
    if (s.drops == 0) {
      s.dropSince = millis();
      Serial.println(F("[SSE] TX full, dropping events"));
    }
    if (s.drops < 0xFF) s.drops++;
    if (s.drops >= SSE_MAX_DROPS || millis() - s.dropSince >= SSE_STALL_MS) {
      Serial.println(F("[SSE] Client stalled, closing"));
      NetUtils::closeNow(c);
      sseRemove(i);
    }
  }
  _sseLastActivity = millis();
}

// A client back from a run of drops gets "event: resync" (the page may
// refresh what it shows) and Companion is asked for its state again, its
// CCUSYNC lines come back as events. The request is made from
// processSSEClients(), as this runs inside broadcasts (CCUBroadcast's
// included), and CCUBroadcast rate limits it. Drops that began before the
// last REQUESTSYNC or less than CCU_SYNC_INTERVAL after it do not ask
// again: that dump resends the values, or caused the drops (no loop). If
// "resync" does not fit, the next event that does retries it.
static const char kSseResync[] PROGMEM = "event: resync\r\ndata: {}\r\n\r\n";

void WebServer::sseResync(EthernetClient &c, SseClient &s) {
  char ev[sizeof(kSseResync)];
  memcpy_P(ev, kSseResync, sizeof(ev));
  bool dead = false;
  unsigned long t0 = millis();
  while (!NetUtils::tryWriteAll(c, (const uint8_t*)ev, sizeof(ev) - 1, &dead)) {
    if (dead || millis() - t0 >= SSE_TX_WAIT_MS) return;
    wdt_reset();
  }
  Serial.print(F("[SSE] TX ok, events dropped: "));
  Serial.println(s.drops);
  s.drops = 0;
  _sseStale = SSE_STALE_TALLY | SSE_STALE_STATE;   // may have been dropped
  if (!CCUBroadcast::syncSentNear(s.dropSince)) _sseSyncWanted = true;
}

void WebServer::processSSEClients() {
  for (uint8_t i = _sseCount; i-- > 0;) {
    if (!sseValid(i)) continue;
    // The browser sends nothing after the request: drop what arrived
    EthernetClient c(_sse[i].sock);
    NetUtils::drainInput(c, 256, 0);
  }

  sendSSEState();

  // Keepalive to every client after SSE_TIMEOUT without events
  if (_sseCount && millis() - _sseLastActivity >= SSE_TIMEOUT) {
    ClientBufferPrint ev((uint8_t*)_sseBuffer, WEB_SSE_BUFFER_SIZE);
    ev.print(F(": keepalive\r\n\r\n"));
    sseBroadcast(ev);
  }

  // Resync asked by sseResync()
  if (_sseSyncWanted) {
    _sseSyncWanted = false;
    if (_sseCount) CCUBroadcast::requestSync();
  }
}

bool WebServer::hasSSEClient() {
  return _sseCount > 0;
}

// On a change, at most every SSE_STATE_MIN_MS (the last state always goes):
//   data: {"type":"tally","pgm":1,"pvw":2}  (bit 0 = camera 1, vMix state)
//   data: {"type":"state","overrideTally":true,"overrideCCU":true,"vmixConnect":true}
void WebServer::sendSSEState() {
  if (!_sseCount) return;
  unsigned long now = millis();
  if (now - _sseStateAt < SSE_STATE_MIN_MS) return;

  uint8_t pgm, pvw;
  TallyManager::getAir(pgm, pvw);
  uint8_t sw = (TallyManager::getOverride() ? SSE_SW_TALLY : 0) |
               (CCUControl::getOverride() ? SSE_SW_CCU : 0) |
               (VmixConnector::getConnectEnabled() ? SSE_SW_VMIX : 0);
  bool tally = (_sseStale & SSE_STALE_TALLY) || pgm != _ssePgm || pvw != _ssePvw;
  bool state = (_sseStale & SSE_STALE_STATE) || sw != _sseSwitches;
  if (!tally && !state) return;

  ClientBufferPrint ev((uint8_t*)_sseBuffer, WEB_SSE_BUFFER_SIZE);
  if (tally) {
    ev.print(F("data: {\"type\":\"tally\",\"pgm\":"));
    ev.print(pgm);
    ev.print(F(",\"pvw\":"));
    ev.print(pvw);
    ev.print(F("}\r\n\r\n"));
  }
  if (state) {
    ev.print(F("data: {\"type\":\"state\",\"overrideTally\":"));
    ev.print((sw & SSE_SW_TALLY) ? F("true") : F("false"));
    ev.print(F(",\"overrideCCU\":"));
    ev.print((sw & SSE_SW_CCU) ? F("true") : F("false"));
    ev.print(F(",\"vmixConnect\":"));
    ev.print((sw & SSE_SW_VMIX) ? F("true") : F("false"));
    ev.print(F("}\r\n\r\n"));
  }
  _ssePgm = pgm;
  _ssePvw = pvw;
  _sseSwitches = sw;
  _sseStale = 0;
  _sseStateAt = now;
  sseBroadcast(ev);   // a client that drops it gets it again on recovery
}

void WebServer::sendSSEEvent(int cameraId, const char* paramKey, const char* value) {
  if (!_sseCount) return;

  // Format: data: {"cam":1,"key":"gain_db","val":"6"}
  ClientBufferPrint ev((uint8_t*)_sseBuffer, WEB_SSE_BUFFER_SIZE);
  ev.print(F("data: {\"cam\":"));
  ev.print(cameraId);
  ev.print(F(",\"key\":\""));
  printJsonSafe(ev, paramKey);   // the key may come from an 8098 client (HTTP-10)
  ev.print(F("\",\"val\":\""));
  printJsonSafe(ev, value);
  ev.print(F("\"}\r\n\r\n"));   // empty line marks end of event
  sseBroadcast(ev);
}

void WebServer::sendSSEPresetLoaded(int cameraId, int presetId, const char* presetName) {
  if (!_sseCount) return;

  // Format: data: {"type":"presetLoaded","cam":1,"preset":0,"name":"Default"}
  ClientBufferPrint ev((uint8_t*)_sseBuffer, WEB_SSE_BUFFER_SIZE);
  ev.print(F("data: {\"type\":\"presetLoaded\",\"cam\":"));
  ev.print(cameraId);
  ev.print(F(",\"preset\":"));
  ev.print(presetId);
  ev.print(F(",\"name\":\""));
  printJsonSafe(ev, presetName);
  ev.print(F("\"}\r\n\r\n"));
  sseBroadcast(ev);
}

void WebServer::sendSSEPresetSaved(int cameraId, int presetId, const char* presetName) {
  if (!_sseCount) return;

  // Format: data: {"type":"presetSaved","cam":1,"preset":0,"name":"My Preset"}
  ClientBufferPrint ev((uint8_t*)_sseBuffer, WEB_SSE_BUFFER_SIZE);
  ev.print(F("data: {\"type\":\"presetSaved\",\"cam\":"));
  ev.print(cameraId);
  ev.print(F(",\"preset\":"));
  ev.print(presetId);
  ev.print(F(",\"name\":\""));
  printJsonSafe(ev, presetName);
  ev.print(F("\"}\r\n\r\n"));
  sseBroadcast(ev);
}

void WebServer::sendSSERequestSync() {
  if (!_sseCount) return;

  // Request web client to send its cached state
  ClientBufferPrint ev((uint8_t*)_sseBuffer, WEB_SSE_BUFFER_SIZE);
  ev.print(F("data: {\"type\":\"requestSync\"}\r\n\r\n"));
  sseBroadcast(ev);
}
