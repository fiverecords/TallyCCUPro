/*
 * webserver.cpp
 * Web server for control and configuration
 *
 * TallyCCU Pro V3.8
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

// ============================================================
// STATIC VARIABLE INITIALIZATION
// ============================================================

EthernetServer WebServer::_server(HTTP_PORT);

// SSE clients
WebServer::SseClient WebServer::_sse[MAX_SSE_CLIENTS];
uint8_t WebServer::_sseCount = 0;
unsigned long WebServer::_sseLastActivity = 0;
bool WebServer::_sseSyncWanted = false;

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

// Response writer over _requestBuffer (the request line is no longer needed)
#define RESPONSE(out, client) \
  ClientBufferPrint out(client, (uint8_t*)_requestBuffer, WEB_REQUEST_BUFFER_SIZE, WEB_RESPONSE_MS)

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

static bool isSystemFile(const char* name) {
  return strcmp_P(name, kIndexHtml) == 0 || strcmp_P(name, kTallyHtml) == 0 ||
         strcmp_P(name, kSdcardHtml) == 0 || strcmp_P(name, kSafemodeHtml) == 0;
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

static unsigned long msLeft(unsigned long deadline) {
  int32_t t = (int32_t)((uint32_t)deadline - (uint32_t)millis());
  return t > 0 ? (unsigned long)t : 0;
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
// HELPER FUNCTIONS
// ============================================================

int WebServer::findChar(const char* str, char c, int start) {
  if (!str) return -1;
  for (int i = start; str[i] != '\0'; i++) {
    if (str[i] == c) return i;
  }
  return -1;
}

void WebServer::extractSubstring(const char* src, char* dest, int start, int end, int maxLen) {
  if (!src || !dest || start < 0 || end <= start) {
    if (dest) dest[0] = '\0';
    return;
  }
  int len = end - start;
  if (len >= maxLen) len = maxLen - 1;
  strncpy(dest, src + start, len);
  dest[len] = '\0';
}

bool WebServer::isNumeric(const char* str) {
  if (!str || str[0] == '\0') return false;
  bool hasDecimal = false;
  int i = 0;
  if (str[0] == '-') i = 1;
  for (; str[i] != '\0'; i++) {
    if (str[i] == '.') {
      if (hasDecimal) return false;
      hasDecimal = true;
      continue;
    }
    if (!isdigit(str[i])) return false;
  }
  return true;
}

void WebServer::urlDecodeInPlace(char* str) {
  if (!str) return;
  int readPos = 0;
  int writePos = 0;
  while (str[readPos] != '\0') {
    if (str[readPos] == '%' && str[readPos + 1] != '\0' && str[readPos + 2] != '\0') {
      char hex[3] = { str[readPos + 1], str[readPos + 2], '\0' };
      str[writePos] = (char)strtol(hex, NULL, 16);
      readPos += 3;
    } else if (str[readPos] == '+') {
      str[writePos] = ' ';
      readPos++;
    } else {
      str[writePos] = str[readPos];
      readPos++;
    }
    writePos++;
  }
  str[writePos] = '\0';
}

// Print a string to any Print target (client, Serial) with JSON escaping
void WebServer::printJsonSafe(Print &out, const char* str) {
  if (!str) return;
  for (int i = 0; str[i] != '\0'; i++) {
    char c = str[i];
    if (c == '"' || c == '\\') {
      out.print('\\');
    }
    out.print(c);
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

  for (uint8_t i = 0; i < count; i++) {
    uint8_t bit = (uint8_t)(1 << i);
    EthernetClient c(i);
    uint8_t sr = c.status();
    if (sr == SnSR::CLOSED) freeSocks++;

    bool http = (EthernetServer::server_port[i] == HTTP_PORT);
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

  // Request line and headers share one total budget
  unsigned long deadline = NetUtils::deadlineIn(WEB_HEADER_MS);
  NetUtils::LineReader lr(_requestBuffer, WEB_REQUEST_BUFFER_SIZE);
  int n = readLinePolled(client, lr, deadline, NULL);
  if (n == 0) {
    // one stray CRLF before the request line is allowed
    n = readLinePolled(client, lr, deadline, NULL);
  }
  if (n <= 0) {
    NetUtils::closeGracefully(client, 0);   // FIN, no wait for the peer
    return;
  }

  Request req;
  readHeaders(client, deadline, req);
  wdt_reset();

  if (hasPrefixP(_requestBuffer, PSTR("GET /events"))) {
    if (handleSSEConnection(client)) return;  // kept open
  } else if (hasPrefixP(_requestBuffer, PSTR("POST "))) {
    processPOSTRequest(_requestBuffer, client, req);
  } else if (hasPrefixP(_requestBuffer, PSTR("GET "))) {
    processGETRequest(_requestBuffer, client, req.gzip);
  } else {
    sendBareStatus(client, 501);
  }

  // Unread input would turn the close into a reset
  NetUtils::drainInput(client, WEB_DRAIN_MAX, 0);
  NetUtils::closeGracefully(client, WEB_CLOSE_MS);
}

// Header lines up to the empty line (or the deadline). Keeps what the
// handlers need; the multipart end pattern goes to _valueBuffer.
void WebServer::readHeaders(EthernetClient &client, unsigned long deadline, Request &req) {
  req.contentLength = -1;
  req.gzip = false;
  req.boundary = false;

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
    } else if (strncasecmp_P(line, PSTR("Content-Type:"), 13) == 0) {
      char* b = strcasestr_P(line + 13, PSTR("boundary="));
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

// Writes len bytes before the deadline in WEB_POLL_MS slices (a stalled
// peer must not stop the tally). False if the budget ran out or the peer
// is gone: close the client.
bool WebServer::writePolled(EthernetClient &client, const uint8_t* buf, size_t len,
                            unsigned long deadline) {
  size_t done = 0;
  for (;;) {
    unsigned long slice = msLeft(deadline);
    if (slice > WEB_POLL_MS) slice = WEB_POLL_MS;
    done += NetUtils::writeWithDeadline(client, buf + done, len - done, slice);
    if (done >= len) return true;
    if (!NetUtils::sendable(client) || NetUtils::expired(deadline)) return false;
    transferPoll();
  }
}

// Waits (bounded) until the socket has no NetUtils SEND in flight, before
// a library write
bool WebServer::waitTxIdle(EthernetClient &client) {
  unsigned long deadline = NetUtils::deadlineIn(WEB_RESPONSE_MS);
  while (!NetUtils::txIdle(client)) {
    wdt_reset();
    if (!NetUtils::sendable(client) || NetUtils::expired(deadline)) return false;
    transferPoll();
  }
  return true;
}

// ============================================================
// PROCESS POST - FOR SAVING PRESETS
// ============================================================

void WebServer::processPOSTRequest(const char* reqLine, EthernetClient &client, const Request &req) {
#if DEBUG_HOT
  Serial.println(F("[POST] Received"));
#endif

  // Safe mode exit
  if (strstr_P(reqLine, PSTR("POST /safemode-exit")) != NULL) {
    drainBody(client, req.contentLength);
    handleSafeModeExit(client);
    return;
  }

  // Check if savePreset
  if (strstr_P(reqLine, PSTR("POST /savePreset")) != NULL) {
    handleSavePresetPOST(client, req.contentLength);
    return;
  }

  // Sync state from web to TCP clients
  if (strstr_P(reqLine, PSTR("POST /syncState")) != NULL) {
    handleSyncStatePOST(client, req.contentLength);
    return;
  }

  // File upload
  if (strstr_P(reqLine, PSTR("POST /upload")) != NULL) {
    handleUploadFile(client, req);
    return;
  }

  // Unrecognized POST
  drainBody(client, req.contentLength);
  sendBareStatus(client, 404);
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
      // 95 encoded bytes decode to >= 31 bytes: overflow only loses the tail
      urlDecodeInPlace(_valueBuffer);
      clampPresetName(_valueBuffer, sizeof(presetName));
      strcpy(presetName, _valueBuffer);
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

  if (!foundData || cameraId < 1 || cameraId > MAX_CAMERAS || presetId < 0 || presetId >= MAX_PRESETS) {
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
  SD.remove(tmpName);   // leftover of an interrupted save

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
  int skipped = 0;   // over-long params: dropped whole, never cut

  for (;;) {
    int ch = in.read();
    if (ch < 0) break;
    char c = (char)ch;

    // URL decode; the two hex digits may arrive in a later segment
    if (c == '%' && in.left() >= 2) {
      int h1 = in.read();
      int h2 = (h1 < 0) ? -1 : in.read();
      if (h2 < 0) {
        cutEscape = true;
        break;
      }
      char hex[3] = { (char)h1, (char)h2, '\0' };
      c = (char)strtol(hex, NULL, 16);
    } else if (c == '+') {
      c = ' ';
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

  bool complete = !cutEscape && in.left() == 0;

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

  if (writeOk && complete) {
    // SdFat rename() fails if the target exists
    char* finalName = _keyBuffer;
    SdUtils::getPresetFilename(finalName, WEB_KEY_BUFFER_SIZE, cameraId, presetId, false);
    if (SD.exists(finalName) && !SD.remove(finalName)) writeOk = false;
    else if (!SD.rename(tmpName, finalName)) writeOk = false;
  }

  if (!writeOk || !complete) {
    // A partial preset would load only some values: the old one is kept.
    // Nothing to drain: incomplete means the body stopped.
    SD.remove(tmpName);
    Serial.print(F("Preset save failed: "));
    Serial.println(tmpName);
    if (!writeOk) {
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

  while (remaining > 0) {
    transferPoll();   // a long body that keeps flowing
    int len = readLinePolled(client, lr, deadline, &remaining);
    if (len < 0) break;   // deadline, idle or disconnect
    if (len == 0 || lr.truncated()) continue;

    // Parse "cam:key:value"
    char* firstColon = strchr(_lineBuffer, ':');
    if (!firstColon) continue;

    *firstColon = '\0';
    int cameraId = atoi(_lineBuffer);

    char* secondColon = strchr(firstColon + 1, ':');
    if (!secondColon) continue;

    *secondColon = '\0';
    const char* paramKey = firstColon + 1;
    const char* value = secondColon + 1;

    if (cameraId >= 1 && cameraId <= MAX_CAMERAS && strlen(paramKey) > 0) {
      // Broadcast to TCP clients (Companion)
      CCUBroadcast::sendParamChange(cameraId, paramKey, value);
      paramCount++;
    }
  }

#if DEBUG_HOT
  Serial.print(F("[syncState] Forwarded "));
  Serial.print(paramCount);
  Serial.println(F(" params to TCP"));
#endif

  sendJSONResponse(client, 200, true, F("OK"), paramCount);
}

// ============================================================
// SEND RESPONSES
// ============================================================

// Status line and headers (JSON responses also carry CORS)
void WebServer::sendHeaders(Print &out, int httpCode, bool json) {
  out.print(F("HTTP/1.1 "));
  out.print(httpCode);
  switch (httpCode) {
    case 200: out.print(F(" OK\r\n")); break;
    case 400: out.print(F(" Bad Request\r\n")); break;
    case 404: out.print(F(" Not Found\r\n")); break;
    case 503: out.print(F(" Service Unavailable\r\n")); break;
    default:  out.print(F(" Internal Server Error\r\n")); break;
  }
  if (json) {
    out.print(F("Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n"));
  } else {
    out.print(F("Content-Type: text/plain\r\n"));
  }
  out.print(F("Connection: close\r\n\r\n"));
}

// Status line only (404 for an unknown POST, 501 for other methods)
void WebServer::sendBareStatus(EthernetClient &client, int httpCode) {
  RESPONSE(out, client);
  out.print(httpCode == 404 ? F("HTTP/1.1 404 Not Found\r\n") : F("HTTP/1.1 501 Not Implemented\r\n"));
  out.print(F("Connection: close\r\n\r\n"));
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

// SD page or file; SdUtils writes the whole response (404 included)
void WebServer::serveSdFile(EthernetClient &client, PGM_P filename, bool gzip) {
  strcpy_P(_tempBuffer, filename);
  SdUtils::serveFile(client, _tempBuffer, gzip);
}

// ============================================================
// PROCESS GET - MAIN FUNCTIONALITY
// ============================================================

void WebServer::processGETRequest(char* reqLine, EthernetClient &client, bool gzip) {

  // ============================================================
  // SAFE MODE HANDLING - CHECK FIRST
  // ============================================================

  // Safe mode status API (always available)
  if (hasPrefixP(reqLine, PSTR("GET /safemode-status"))) {
    handleSafeModeStatus(client);
    return;
  }

  // In safe mode, redirect main pages to diagnostic page
  if (SafeMode::isActive()) {
    if (hasPrefixP(reqLine, PSTR("GET / ")) ||
        hasPrefixP(reqLine, PSTR("GET /index")) ||
        hasPrefixP(reqLine, PSTR("GET /ccu"))) {
      serveSdFile(client, kSafemodeHtml, gzip);
      return;
    }
  }

  // ============================================================
  // NORMAL REQUEST HANDLING
  // ============================================================

  // Reboot
  if (hasPrefixP(reqLine, PSTR("GET /reboot"))) {
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

  // getOverrides
  if (hasPrefixP(reqLine, PSTR("GET /?getOverrides"))) {
    handleGetOverrides(client);
    return;
  }

  // getTallyMap - Return current tally input->camera mappings
  if (hasPrefixP(reqLine, PSTR("GET /?getTallyMap"))) {
    handleGetTallyMap(client);
    return;
  }

  // getVmixConnect - vMix connection status (con IP)
  if (hasPrefixP(reqLine, PSTR("GET /?getVmixConnect"))) {
    handleGetVmixConnect(client);
    return;
  }

  // getParams - Legacy endpoint (SSE is now primary sync method)
  // Returns empty - web uses SSE for real-time state
  if (hasPrefixP(reqLine, PSTR("GET /?getParams="))) {
    int cameraId = atoi(reqLine + 16);
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

  // ============================================================
  // SD FILE MANAGEMENT
  // ============================================================

  // listFiles - List files on SD
  if (hasPrefixP(reqLine, PSTR("GET /?listFiles"))) {
    handleListFiles(client);
    return;
  }

  // download - Download file
  if (hasPrefixP(reqLine, PSTR("GET /?download="))) {
    int spacePos = findChar(reqLine, ' ', 15);
    if (spacePos > 15) {
      extractSubstring(reqLine, _tempBuffer, 15, spacePos, WEB_TEMP_BUFFER_SIZE);
      urlDecodeInPlace(_tempBuffer);
      handleDownloadFile(client, _tempBuffer);
    } else {
      send404(client);
    }
    return;
  }

  // deleteFile - Delete file
  if (hasPrefixP(reqLine, PSTR("GET /?deleteFile="))) {
    int spacePos = findChar(reqLine, ' ', 17);
    if (spacePos > 17) {
      extractSubstring(reqLine, _tempBuffer, 17, spacePos, WEB_TEMP_BUFFER_SIZE);
      urlDecodeInPlace(_tempBuffer);
      handleDeleteFile(client, _tempBuffer);
    } else {
      send404(client);
    }
    return;
  }

  // renameFile - Rename file: /?renameFile=oldname&to=newname
  // (decoded in place in the request line)
  if (hasPrefixP(reqLine, PSTR("GET /?renameFile="))) {
    int spacePos = findChar(reqLine, ' ', 17);
    if (spacePos > 17) {
      char* arg = reqLine + 17;
      reqLine[spacePos] = '\0';
      urlDecodeInPlace(arg);

      char* toPtr = strstr_P(arg, PSTR("&to="));
      if (toPtr) {
        *toPtr = '\0';
        handleRenameFile(client, arg, toPtr + 4);
      } else {
        sendJSONResponse(client, 200, false, F("Missing newname"));
      }
    } else {
      send404(client);
    }
    return;
  }

  // fileExists - Check if file exists
  if (hasPrefixP(reqLine, PSTR("GET /?fileExists="))) {
    int spacePos = findChar(reqLine, ' ', 17);
    if (spacePos > 17) {
      extractSubstring(reqLine, _tempBuffer, 17, spacePos, WEB_TEMP_BUFFER_SIZE);
      urlDecodeInPlace(_tempBuffer);
      sendJSONBool(client, PSTR("exists"), SD.exists(_tempBuffer));
    } else {
      send404(client);
    }
    return;
  }

  // listPresets
  if (hasPrefixP(reqLine, PSTR("GET /?listPresets"))) {
#if DEBUG_HOT
    Serial.println(F("Listing presets"));
#endif
    listPresetsAsJSON(client);
    return;
  }

  // Main page
  if (hasPrefixP(reqLine, PSTR("GET / ")) || hasPrefixP(reqLine, PSTR("GET /index.html"))) {
    serveSdFile(client, kIndexHtml, gzip);
    return;
  }

  // Tally page
  if (hasPrefixP(reqLine, PSTR("GET /tally.html"))) {
    serveSdFile(client, kTallyHtml, gzip);
    return;
  }

  // SD management page (direct access)
  if (hasPrefixP(reqLine, PSTR("GET /sd"))) {
    serveSdFile(client, kSdcardHtml, gzip);
    return;
  }

  // Static files. serveFile() writes the whole response, its 404/503
  // included (HTTP-12); a name too long for _tempBuffer is a 404 below.
  if (hasPrefixP(reqLine, PSTR("GET /")) && !hasPrefixP(reqLine, PSTR("GET /?"))) {
    int spacePos = findChar(reqLine, ' ', 5);
    if (spacePos > 5 && spacePos - 5 < WEB_TEMP_BUFFER_SIZE) {
      extractSubstring(reqLine, _tempBuffer, 5, spacePos, WEB_TEMP_BUFFER_SIZE);
      SdUtils::serveFile(client, _tempBuffer, gzip);
      return;
    }
  }

  // Query string, parsed in place in the request line
  if (!hasPrefixP(reqLine, PSTR("GET /?"))) {
    send404(client);
    return;
  }

  char* query = reqLine + 6;
  int spacePos = findChar(query, ' ', 0);
  if (spacePos < 0) {
    send404(client);
    return;
  }
  query[spacePos] = '\0';

  // loadPreset con returnValues
  if (strncmp_P(query, PSTR("loadPreset="), 11) == 0) {
    handleLoadPreset(client, query + 11);
    return;
  }

  // Parse normal parameters. Every valid pair is applied; the first
  // error decides the answer (200 OK, 400 or 503 "ERROR <reason>").
  ParamContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ParamResult result = PARAM_OK;
  int idx = 0;
  int queryLen = spacePos;

  while (idx < queryLen) {
    int ampPos = findChar(query, '&', idx);
    int pairEnd = (ampPos < 0) ? queryLen : ampPos;

    if (pairEnd > idx) {   // skip empty pairs ("&&", trailing '&')
      int eqPos = findChar(query, '=', idx);
      ParamResult r;
      if (eqPos <= idx || eqPos >= pairEnd) {
        r = PARAM_ERR_KEY;      // empty key or no '='
      } else if (eqPos - idx >= WEB_KEY_BUFFER_SIZE || pairEnd - eqPos - 1 >= WEB_VALUE_BUFFER_SIZE) {
        r = PARAM_ERR_LENGTH;   // never act on a truncated key or value
      } else {
        extractSubstring(query, _keyBuffer, idx, eqPos, WEB_KEY_BUFFER_SIZE);
        extractSubstring(query, _valueBuffer, eqPos + 1, pairEnd, WEB_VALUE_BUFFER_SIZE);
        r = handleParam(_keyBuffer, _valueBuffer, ctx);
      }
      if (r != PARAM_OK) {
        if (r != PARAM_ERR_SAFE_MODE) ctx.badRequest = true;
        if (result == PARAM_OK) result = r;
      }
    }

    idx = (ampPos < 0) ? queryLen : ampPos + 1;
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

void WebServer::handleLoadPreset(EthernetClient &client, char* args) {
  char* rv = strstr_P(args, PSTR("&returnValues="));
  bool returnValues = (rv != NULL && rv[14] == '1');
  char* amp = strchr(args, '&');
  if (amp) *amp = '\0';

  // "cam,idx", both strict decimal
  long cameraId = -1;
  long presetId = -1;
  char* comma = strchr(args, ',');
  if (comma) {
    *comma = '\0';
    cameraId = parseIndexed(args, PSTR(""), PSTR(""));
    presetId = parseIndexed(comma + 1, PSTR(""), PSTR(""));
  }

  if (cameraId < 1 || cameraId > MAX_CAMERAS || presetId < 0 || presetId >= MAX_PRESETS) {
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
// ENVIAR JSON CON VALORES DEL PRESET
// ============================================================

// Only called after a successful applyPreset: always "success":true
void WebServer::sendJSONPresetValues(EthernetClient &client, int cameraId, int presetId) {
  SdUtils::getPresetFilename(_tempBuffer, WEB_TEMP_BUFFER_SIZE, cameraId, presetId);
  File presetFile = SD.open(_tempBuffer);

  RESPONSE(out, client);
  sendHeaders(out, 200, true);

  if (!presetFile) {
    out.print(F("{\"success\":true,\"parameters\":{}}\r\n"));
    out.flush();
    return;
  }

  out.print(F("{\"success\":true,\"parameters\":{"));
  bool firstParam = true;

  while (presetFile.available() && !out.failed()) {
    wdt_reset();
    int len = 0;
    while (presetFile.available() && len < WEB_LINE_BUFFER_SIZE - 1) {
      char c = presetFile.read();
      if (c == '\n' || c == '\r') break;
      _lineBuffer[len++] = c;
    }
    _lineBuffer[len] = '\0';

    if (len == 0) continue;

    int colonPos = findChar(_lineBuffer, ':', 0);
    if (colonPos <= 0) continue;

    extractSubstring(_lineBuffer, _keyBuffer, 0, colonPos, WEB_KEY_BUFFER_SIZE);
    extractSubstring(_lineBuffer, _valueBuffer, colonPos + 1, len, WEB_VALUE_BUFFER_SIZE);
    urlDecodeInPlace(_valueBuffer);

    if (strcmp_P(_keyBuffer, PSTR("cameraId")) == 0 || strcmp_P(_keyBuffer, PSTR("presetId")) == 0) {
      continue;
    }

    if (!firstParam) out.print(',');
    firstParam = false;

    out.print('"');
    out.print(_keyBuffer);
    out.print(F("\":"));

    if (strchr(_valueBuffer, ',') != NULL) {
      out.print('[');
      const char delim[2] = { ',', '\0' };
      char* token = strtok(_valueBuffer, delim);
      bool firstVal = true;
      while (token != NULL) {
        if (!firstVal) out.print(',');
        firstVal = false;
        out.print(token);
        token = strtok(NULL, delim);
      }
      out.print(']');
    } else {
      if (isNumeric(_valueBuffer)) {
        out.print(_valueBuffer);
      } else {
        out.print('"');
        printJsonSafe(out, _valueBuffer);
        out.print('"');
      }
    }
  }

  out.print(F("}}"));
  out.flush();
  presetFile.close();
}

// ============================================================
// HANDLE GET PARAMETERS
// ============================================================

WebServer::ParamResult WebServer::handleParam(const char* key, char* value, ParamContext &ctx) {
  wdt_reset();
  urlDecodeInPlace(value);

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
  // SdUtils writes the body through the library: no NetUtils SEND in flight
  if (waitTxIdle(client)) SdUtils::writePresetListToClient(client);
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

void WebServer::handleListFiles(EthernetClient &client) {
  RESPONSE(out, client);
  sendHeaders(out, 200, true);
  out.print(F("{\"files\":["));

  char rootPath[2] = { '/', '\0' };
  File root = SD.open(rootPath);
  if (root) {
    bool first = true;
    File entry;
    char fileName[32];
    while (!out.failed() && (entry = root.openNextFile())) {
      if (!entry.isDirectory()) {
        entry.getName(fileName, sizeof(fileName));

        // Hide system files
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
    }
    root.close();
  }

  out.print(F("]}\r\n"));
  out.flush();
}

void WebServer::handleDownloadFile(EthernetClient &client, const char* filename) {
  File file = SD.open(filename);
  if (!file) {
    send404(client);
    return;
  }

  unsigned long size = file.size();
  {
    RESPONSE(out, client);
    out.print(F("HTTP/1.1 200 OK\r\nContent-Disposition: attachment; filename=\""));
    out.print(filename);
    out.print(F("\"\r\nContent-Type: application/octet-stream\r\nContent-Length: "));
    out.print(size);
    out.print(F("\r\nConnection: close\r\n\r\n"));
    out.flush();
    if (out.failed()) {
      file.close();
      return;
    }
  }

  // Content in blocks through _requestBuffer, one total budget; the vMix
  // tally keeps running, also while the peer stalls (WDT-02)
  unsigned long deadline = bodyDeadline(size);
  uint8_t* buf = (uint8_t*)_requestBuffer;
  for (;;) {
    wdt_reset();
    transferPoll();
    int n = file.read(buf, WEB_REQUEST_BUFFER_SIZE);
    if (n <= 0 || !writePolled(client, buf, (size_t)n, deadline)) break;
  }
  file.close();
}

void WebServer::handleDeleteFile(EthernetClient &client, const char* filename) {
  // Protect system files
  bool protectedFile = isSystemFile(filename);
  bool success = !protectedFile && SD.exists(filename) && SD.remove(filename);
  if (success) SdUtils::invalidateGzip(filename);

  RESPONSE(out, client);
  sendHeaders(out, 200, true);
  if (protectedFile) {
    out.print(F("{\"success\":false,\"error\":\"protected\"}\r\n"));
  } else {
    out.print(F("{\"success\":"));
    out.print(success ? F("true") : F("false"));
    out.print(F("}\r\n"));
  }
  out.flush();
}

// The names live in the request line: rename first, then answer
void WebServer::handleRenameFile(EthernetClient &client, const char* oldName, const char* newName) {
  bool success = SD.exists(oldName) && !SD.exists(newName) && SD.rename(oldName, newName);
  if (success) {
    SdUtils::invalidateGzip(oldName);
    SdUtils::invalidateGzip(newName);
  }
  sendJSONBool(client, PSTR("success"), success);
}

// Multipart upload, streamed to upload.tmp and renamed over the target
// only when the closing boundary arrived and every write worked (SD-02).
// Errors answer 200 with success:false (the page shows the message).
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
  char* filename = _keyBuffer;
  filename[0] = '\0';

  // Part headers line by line (HTTP-05): the file starts after the empty
  // line ending the headers of a part with filename="...". A name that is
  // cut (long line or longer than _keyBuffer) is rejected, never shortened.
  NetUtils::LineReader lr(_lineBuffer, WEB_LINE_BUFFER_SIZE);
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
    if (_lineBuffer[0] == '-' && _lineBuffer[1] == '-') {
      name = 0;                  // boundary: a new part
      continue;
    }
    char* fnPtr = strstr_P(_lineBuffer, PSTR("filename=\""));
    if (fnPtr) {
      fnPtr += 10;
      uint8_t i = 0;
      while (fnPtr[i] && fnPtr[i] != '"' && i < WEB_KEY_BUFFER_SIZE - 1) {
        filename[i] = fnPtr[i];
        i++;
      }
      filename[i] = '\0';
      name = (fnPtr[i] != '"') ? -1 : (i > 0) ? 1 : 0;
    } else if (lr.truncated() &&
               strncasecmp_P(_lineBuffer, PSTR("Content-Disposition:"), 20) == 0) {
      name = -1;                 // filename= may be past the cut
    }
  }

  if (!inContent) {
    if (!stopped) drainBody(client, remaining);
    sendJSONResponse(client, 200, false, name < 0 ? F("Bad filename") :
                     name > 0 ? F("Incomplete data") : F("No file"));
    return;
  }

  char* tmpName = _tempBuffer;
  strcpy_P(tmpName, kUploadTmp);
  if (strcasecmp(filename, tmpName) == 0) {
    drainBody(client, remaining);
    sendJSONResponse(client, 200, false, F("Bad filename"));
    return;
  }

  SD.remove(tmpName);   // leftover of an interrupted upload
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
  bool ok = found && writeOk;
  if (ok) {
    // SdFat rename() fails if the target exists
    if (SD.exists(filename) && !SD.remove(filename)) ok = false;
    else ok = SD.rename(tmpName, filename);
    writeOk = ok;
    if (ok) SdUtils::invalidateGzip(filename);   // or the old .gz is served
  }
  if (!ok) {
    SD.remove(tmpName);
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
              "Access-Control-Allow-Origin: *\r\n"
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
  out.print(F("\"}\r\n"));
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
                "Access-Control-Allow-Origin: *\r\n"
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
  if (!CCUBroadcast::syncSentNear(s.dropSince)) _sseSyncWanted = true;
}

void WebServer::processSSEClients() {
  for (uint8_t i = _sseCount; i-- > 0;) {
    if (!sseValid(i)) continue;
    // The browser sends nothing after the request: drop what arrived
    EthernetClient c(_sse[i].sock);
    NetUtils::drainInput(c, 256, 0);
  }

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

void WebServer::sendSSEEvent(int cameraId, const char* paramKey, const char* value) {
  if (!_sseCount) return;

  // Format: data: {"cam":1,"key":"gain_db","val":"6"}
  ClientBufferPrint ev((uint8_t*)_sseBuffer, WEB_SSE_BUFFER_SIZE);
  ev.print(F("data: {\"cam\":"));
  ev.print(cameraId);
  ev.print(F(",\"key\":\""));
  ev.print(paramKey);
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
