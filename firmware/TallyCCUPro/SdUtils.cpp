/*
 * SdUtils.cpp
 * SD card utilities and preset management implementation
 *
 * Features:
 * - No String usage in critical functions
 * - Static buffer file reading
 * - Bounded block writes to the client (NetUtils)
 * - Card loss detection and rate-limited remount
 */

#include "SdUtils.h"
#include "CCUBroadcast.h"
#include "webserver.h"
#include "NetUtils.h"
#include "VmixConnector.h"
#include <avr/wdt.h>
#include <utility/w5100.h>

// Static variable initialization
SdFat SdUtils::_sd;
uint8_t SdUtils::_sdBuffer[SD_BUFFER_SIZE];
char SdUtils::_lineBuffer[SD_LINE_BUFFER_SIZE];

// Card state (SD-03)
static bool s_sdOk = false;       // mounted, no card error since
static uint32_t s_mountAt = 0;    // millis() of the last begin()

// Background transfers. The socket index is checked against the peer
// before every use (NetUtils rules: an index is reused once it closes).
enum : uint8_t {
  XF_FREE = 0,
  XF_BODY,      // file open, body being queued
  XF_FIN        // body done or cut: FIN once the data is out
};

struct Xfer {
  File f;
  uint32_t left;            // body bytes not yet queued
  uint32_t rip;
  unsigned long end;        // deadline of the current state
  uint16_t rport;
  uint16_t progressAt;      // last bytes queued (ms, 16 bit)
  uint8_t sock;
  uint8_t state;
};
static Xfer s_xfer[SD_XFERS];

// Sockets whose FIN is out, kept outside the slots until they close or
// SD_SEND_LINGER_MS: the tail of the body may still be retransmitted.
struct Linger {
  uint32_t rip;
  uint16_t rport;           // 0: entry unused
  uint16_t at;              // FIN sent (ms, 16 bit)
  uint8_t sock;
};
static Linger s_linger[SD_LINGERS];

static void abortTransfers(const char* name);

// Keys never applied from a preset (TYPE_VOID actions are skipped by
// CCUControl): velocities/speeds would leave the head or zoom moving and
// color bars go on air. video_mode and memory_preset are filtered by value
// in presetValueAllowed(). The web UI never stores any of them
// (NO_PRESET_PARAMS in sdcard/index.html); Companion presets may.
static const char kSkipPanTilt[] PROGMEM = "pan_tilt_velocity";
static const char kSkipZoomSpeed[] PROGMEM = "set_continuous_zoom_speed";
static const char kSkipColorBars[] PROGMEM = "color_bars_display_time_seconds";
static PGM_P const kPresetSkipKeys[] PROGMEM = {
  kSkipPanTilt, kSkipZoomSpeed, kSkipColorBars
};

// video_mode is applied (Companion presets may switch format on purpose),
// except "60,0,6,0,0": the never-read default that the web UI (before v3.8)
// and the Companion module store when the format was never set, which
// switched cameras to UHD 60p on load.
// memory_preset: recall ("2,N") is applied as in v3.7.1; store ("1,N") is
// dropped, it would overwrite the PTZ memory with the current position.
static bool presetValueAllowed(const char* key, const char* value) {
  float v[5];
  if (strcmp_P(key, PSTR("video_mode")) == 0) {
    if (CCUControl::parseFloatList(value, v, 5) != 5) return true;  // CCU rejects it
    return !(v[0] == 60 && v[1] == 0 && v[2] == 6 && v[3] == 0 && v[4] == 0);
  }
  if (strcmp_P(key, PSTR("memory_preset")) == 0) {
    return CCUControl::parseFloatList(value, v, 2) == 2 && v[0] == 2;
  }
  return true;
}

static bool isPresetSkipKey(const char* key) {
  for (uint8_t i = 0; i < sizeof(kPresetSkipKeys) / sizeof(kPresetSkipKeys[0]); i++) {
    if (strcmp_P(key, (PGM_P)pgm_read_ptr(&kPresetSkipKeys[i])) == 0) return true;
  }
  return false;
}

// ============================================================
// CARD STATE
// ============================================================

// SdFat keeps the card error code until the next begin(): sdErrorCode()
// in 2.x, cardErrorCode() in 1.x. Without either (host emulator): none.
template <class T>
static auto cardErrorOf(T& sd, int) -> decltype(sd.sdErrorCode()) { return sd.sdErrorCode(); }
template <class T>
static auto cardErrorOf(T& sd, long) -> decltype(sd.cardErrorCode()) { return sd.cardErrorCode(); }
template <class T>
static uint8_t cardErrorOf(T&, ...) { return 0; }

// After a failed operation: a card error (pulled card, failed read or
// write) marks the SD unavailable; a missing file does not. True if the
// SD is (now) unavailable.
static bool cardFailed() {
  if (s_sdOk && cardErrorOf(SdUtils::getSd(), 0)) {
    s_sdOk = false;
    Serial.println(F("SD card error"));
  }
  return !s_sdOk;
}

bool SdUtils::begin() {
  digitalWrite(SD_PIN_CS, HIGH);
  pinMode(SD_PIN_CS, OUTPUT);

  // Files of the old mount are not valid after begin()
  abortTransfers(NULL);

  // Up to ~2 s without a card (SdFat init timeout)
  wdt_reset();
  s_sdOk = _sd.begin(SD_PIN_CS, SD_SCK_MHZ(50));
  s_mountAt = millis();
  wdt_reset();

  Serial.println(s_sdOk ? F("SD initialized successfully") : F("Could not initialize SD"));
  return s_sdOk;
}

bool SdUtils::isAvailable() {
  if (!cardFailed()) return true;
  if ((uint32_t)millis() - s_mountAt < SD_REMOUNT_MS) return false;

  Serial.println(F("SD remount"));
  VmixConnector::pollTally();   // begin() can block ~2 s
  begin();
  VmixConnector::pollTally();
  return s_sdOk;
}

SdFat& SdUtils::getSd() {
  return _sd;
}

// Opens f in place where File::open(volume, path, flags) exists (SdFat),
// saving the 64-byte temporary of 'f = sd.open(path)' on the stack
template <class F, class V>
static auto openIn(F& f, V& vol, const char* path, int) -> decltype(f.open(&vol, path, O_RDONLY)) {
  return f.open(&vol, path, O_RDONLY);
}
template <class F, class V>
static bool openIn(F& f, V& vol, const char* path, long) {
  f = vol.open(path);
  return f.isOpen();
}

// Runs a card operation. A failure caused by the card marks it unavailable;
// the SD is then remounted (when due) and the operation retried once, so the
// first use after a reinsert does not fail. False at once while unavailable.
template <class Op>
static bool cardOp(Op op) {
  for (uint8_t attempt = 0; attempt < 2; attempt++) {
    if (!SdUtils::isAvailable()) return false;
    if (op()) return true;
    if (!cardFailed()) return false;   // not there, or refused
  }
  return false;
}

// Opens for reading (file or directory)
static bool openRead(File& f, const char* path) {
  return cardOp([&]() { return openIn(f, SdUtils::getSd(), path, 0); });
}

int SdUtils::readLine(File& f, char* buf, size_t size, bool* cut) {
  size_t len = 0;
  *cut = false;
  if (f.available() <= 0) return -1;
  while (f.available() > 0) {
    int c = f.read();
    if (c < 0) return -2;
    if (c == '\n') break;
    if (c == '\r') {
      if (f.peek() == '\n') f.read();
      break;
    }
    if (len + 1 < size) buf[len++] = (char)c;
    else *cut = true;
  }
  buf[len] = '\0';
  return (int)len;
}

bool SdUtils::fileExists(const char* filename) {
  return cardOp([=]() { return _sd.exists(filename); });
}

bool SdUtils::removeFile(const char* filename) {
  abortTransfers(filename);
  return cardOp([=]() { return _sd.remove(filename); });
}

bool SdUtils::renameFile(const char* oldName, const char* newName) {
  return cardOp([=]() { return _sd.rename(oldName, newName); });
}

bool SdUtils::replaceFile(const char* tmp, const char* dst) {
  abortTransfers(dst);
  if (!cardOp([=]() { return !_sd.exists(dst) || _sd.remove(dst); })) {
    removeFile(tmp);   // dst kept
    return false;
  }
  // dst is gone: if the rename fails, tmp (complete) is the only copy: kept
  return renameFile(tmp, dst);
}

void SdUtils::invalidateGzip(const char* filename) {
  size_t n = strlen(filename);
  if (n + 4 > SD_LINE_BUFFER_SIZE) return;   // serveFile() never looks for it
  if (n >= 3 && strcasecmp_P(filename + n - 3, PSTR(".gz")) == 0) return;
  strcpy(_lineBuffer, filename);
  strcat_P(_lineBuffer, PSTR(".gz"));
  removeFile(_lineBuffer);   // usually not there
}

void SdUtils::getPresetFilename(char* buffer, int bufSize, int cameraId, int presetId, bool isTemp) {
  snprintf_P(buffer, bufSize, isTemp ? PSTR("preset_%d_%d.tmp") : PSTR("preset_%d_%d.dat"),
             cameraId, presetId);
}

// ============================================================
// SENDING
// ============================================================

// Sends len bytes within ms. While the peer is slow, WebServer::transferPoll()
// keeps the vMix tally, the PTZ deadman and the port-80 listener running.
static bool sendBlock(EthernetClient &client, const uint8_t* buf, size_t len, uint32_t ms) {
  uint32_t start = millis();
  size_t done = 0;
  for (;;) {
    uint32_t spent = (uint32_t)millis() - start;
    uint32_t slice = ms > spent ? ms - spent : 0;
    if (slice > SD_SEND_SLICE_MS) slice = SD_SEND_SLICE_MS;
    done += NetUtils::writeWithDeadline(client, buf + done, len - done, slice);
    if (done >= len) return true;
    if (slice == 0 || !NetUtils::sendable(client)) return false;
    WebServer::transferPoll();
  }
}

// Lets the last SEND complete within ms (transferPoll() meanwhile), so that
// the caller's close (or a library write) finds the socket idle
static void waitSent(EthernetClient &client, uint32_t ms) {
  uint32_t t0 = millis();
  while (!NetUtils::txIdle(client) && NetUtils::sendable(client) &&
         (uint32_t)millis() - t0 < ms) {
    wdt_reset();
    WebServer::transferPoll();
  }
}

// Time left until the absolute deadline 'end', at most SD_SEND_BLOCK_MS
static uint32_t blockMs(unsigned long end) {
  int32_t t = (int32_t)((uint32_t)end - (uint32_t)millis());
  if (t <= 0) return 0;
  return (uint32_t)t < SD_SEND_BLOCK_MS ? (uint32_t)t : SD_SEND_BLOCK_MS;
}

// ============================================================
// PRESETS
// ============================================================

// True only if the file was opened and fully read. Lines with unknown keys
// or malformed values are skipped (counted) but do not fail the preset.
bool SdUtils::applyPreset(int cameraId, int presetId) {
  // Safe Mode / shield not initialized: no I2C writes
  if (!CCUControl::isReady()) {
    return false;
  }

  if (cameraId < 1 || cameraId > MAX_CAMERAS || presetId < 0) {
    return false;
  }

  // No SD lock: everything runs in loop(), and nothing reached from here
  // (the tally poll included) touches the SD (SD-07)
  char filename[SD_PATH_BUFFER_SIZE];
  getPresetFilename(filename, SD_PATH_BUFFER_SIZE, cameraId, presetId, false);

  File presetFile;
  if (!openRead(presetFile, filename) || !presetFile.isFile()) {
    presetFile.close();
    Serial.print(s_sdOk ? F("Preset not found: ") : F("SD not available: "));
    Serial.println(filename);
    return false;
  }

  Serial.print(F("Applying preset to camera "));
  Serial.println(cameraId);

  // Buffer for preset name
  char presetName[32];
  strcpy_P(presetName, PSTR("Preset"));
  bool readError = false;
  int skipped = 0;

  // Read line by line to static buffer
  for (uint8_t n = 1; ; n++) {
    wdt_reset();
    if (n % 8 == 0) VmixConnector::pollTally();   // I2C writes add up
    bool overflow;
    int len = readLine(presetFile, _lineBuffer, SD_LINE_BUFFER_SIZE, &overflow);
    if (len == -1) break;
    if (len < 0) {
      readError = true;
      break;
    }

    // A truncated value could still parse as a wrong number: drop it
    if (overflow) {
      skipped++;
      continue;
    }

    if (len == 0) continue;

    char* colonPos = strchr(_lineBuffer, ':');
    if (!colonPos) continue;

    *colonPos = '\0';
    char* paramKey = _lineBuffer;
    char* paramValue = colonPos + 1;

    // Capture preset name (stored decoded)
    if (strcmp_P(paramKey, PSTR("name")) == 0) {
      strncpy(presetName, paramValue, 31);
      presetName[31] = '\0';
      continue;
    }

    // Skip other metadata
    if (strcmp_P(paramKey, PSTR("cameraId")) == 0 ||
        strcmp_P(paramKey, PSTR("presetId")) == 0) {
      continue;
    }

    if (isPresetSkipKey(paramKey) || !presetValueAllowed(paramKey, paramValue)) {
#if DEBUG_HOT
      Serial.print(F("[Preset] skip "));
      Serial.println(paramKey);
#endif
      continue;
    }

    if (!CCUControl::applyParameterByKey(paramKey, paramValue, cameraId, true)) {
      skipped++;
#if DEBUG_HOT
      Serial.print(F("[Preset] rejected "));
      Serial.println(paramKey);
#endif
    }
  }

  presetFile.close();

  if (readError) {
    cardFailed();
    Serial.print(F("Error reading preset: "));
    Serial.println(filename);
    return false;
  }

  // Notify TCP clients (Companion)
  CCUBroadcast::sendPresetLoaded(cameraId, presetId, presetName);

  // Notify SSE client (web)
  WebServer::sendSSEPresetLoaded(cameraId, presetId, presetName);

  Serial.print(F("Preset applied"));
  if (skipped > 0) {
    Serial.print(F(", lines rejected: "));
    Serial.print(skipped);
  }
  Serial.println();
  return true;
}

// "preset_<cam>_<id>.dat" exactly
static bool parsePresetFilename(const char* name, int* cam, int* id) {
  if (strncmp_P(name, PSTR("preset_"), 7) != 0) return false;
  const char* p = name + 7;
  if (!isdigit((unsigned char)*p)) return false;
  char* end;
  long c = strtol(p, &end, 10);
  if (*end != '_' || !isdigit((unsigned char)end[1])) return false;
  p = end + 1;
  long i = strtol(p, &end, 10);
  if (strcmp_P(end, PSTR(".dat")) != 0) return false;
  *cam = (int)c;
  *id = (int)i;
  return true;
}

// One entry fits in this: comma, two ints, fixed fields and a name of 63
// characters escaped to at most 126
#define SD_LIST_ENTRY_MAX 200
static_assert(SD_BUFFER_SIZE >= SD_LIST_ENTRY_MAX + 16, "SD_BUFFER_SIZE too small for the preset list");

// Sends what out holds (by the absolute deadline 'end') and empties it
static bool listFlush(EthernetClient &client, ClientBufferPrint &out, unsigned long end) {
  bool ok = !out.failed() && sendBlock(client, out.data(), out.length(), blockMs(end));
  out.clear();
  return ok;
}

bool SdUtils::writePresetListToClient(EthernetClient &client) {
  // Composed in _sdBuffer and sent block by block: a slow reader is waited
  // for in slices with transferPoll() in between, SD_LIST_MS in total
  ClientBufferPrint out(_sdBuffer, SD_BUFFER_SIZE);
  unsigned long end = NetUtils::deadlineIn(SD_LIST_MS);
  out.print(F("{\"presets\":["));

  // The walk stops early on a huge directory; the rest of the budget is
  // left for the last blocks
  unsigned long walkEnd = NetUtils::deadlineIn(SD_LIST_WALK_MS);
  bool truncated = false;
  bool ok = true;

  File root;
  if (openRead(root, "/")) {
    bool firstPreset = true;
    for (uint8_t n = 1; ; n++) {
      wdt_reset();
      WebServer::transferPoll();   // the walk can take SD_LIST_WALK_MS
      if (NetUtils::expired(walkEnd)) {
        truncated = true;
        break;
      }

      File entry = root.openNextFile();
      if (!entry) break;

      int cameraId, presetId;
      entry.getName(_lineBuffer, SD_LINE_BUFFER_SIZE);
      if (entry.isDir() || !parsePresetFilename(_lineBuffer, &cameraId, &presetId)) {
        entry.close();
        continue;
      }

      // "name:" is the third line; look at the first few only
      bool named = false;
      for (uint8_t line = 0; line < 4 && !named; line++) {
        bool cut;
        if (readLine(entry, _lineBuffer, SD_LINE_BUFFER_SIZE, &cut) < 0) break;
        named = strncmp_P(_lineBuffer, PSTR("name:"), 5) == 0;
      }
      entry.close();

      // The name stays in _lineBuffer meanwhile
      if (out.availableForWrite() < SD_LIST_ENTRY_MAX) {
        ok = listFlush(client, out, end);
        if (!ok) break;
      }

      if (!firstPreset) out.print(',');
      firstPreset = false;

      out.print(F("{\"cameraId\":"));
      out.print(cameraId);
      out.print(F(",\"presetId\":"));
      out.print(presetId);
      out.print(F(",\"name\":\""));
      if (named) {
        char* name = _lineBuffer + 5;
        name[63] = '\0';   // as before: at most 63 stored characters
        WebServer::printJsonSafe(out, name);   // stored decoded (SD-05)
      } else {
        out.print(F("Preset"));
      }
      out.print(F("\"}"));
    }
    root.close();
  }

  if (ok && out.availableForWrite() < SD_LIST_ENTRY_MAX) ok = listFlush(client, out, end);
  if (ok) {
    out.print(']');
    if (cardFailed()) out.print(F(",\"error\":\"sd\""));
    if (truncated) out.print(F(",\"truncated\":true"));
    out.print(F("}\r\n"));
    ok = listFlush(client, out, end);
  }

  if (!ok) {
    NetUtils::closeNow(client);
    return false;
  }
  waitSent(client, blockMs(end));
  return true;
}

// ============================================================
// WEB FILES
// ============================================================

// Content-Type from the file extension (case-insensitive, as FAT names)
static PGM_P contentType(const char* name) {
  const char* ext = strrchr(name, '.');
  if (ext) {
    ext++;
    if (!strcasecmp_P(ext, PSTR("html")) || !strcasecmp_P(ext, PSTR("htm")))
      return PSTR("text/html; charset=UTF-8");
    if (!strcasecmp_P(ext, PSTR("css"))) return PSTR("text/css");
    if (!strcasecmp_P(ext, PSTR("js"))) return PSTR("application/javascript");
    if (!strcasecmp_P(ext, PSTR("json"))) return PSTR("application/json");
    if (!strcasecmp_P(ext, PSTR("png"))) return PSTR("image/png");
    if (!strcasecmp_P(ext, PSTR("jpg")) || !strcasecmp_P(ext, PSTR("jpeg")))
      return PSTR("image/jpeg");
    if (!strcasecmp_P(ext, PSTR("ico"))) return PSTR("image/x-icon");
    if (!strcasecmp_P(ext, PSTR("svg"))) return PSTR("image/svg+xml");
    if (!strcasecmp_P(ext, PSTR("woff2"))) return PSTR("font/woff2");
    if (!strcasecmp_P(ext, PSTR("woff"))) return PSTR("font/woff");
  }
  return PSTR("text/plain");
}

// Short error answers
static const char kResp404[] PROGMEM =
  "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
  "Content-Length: 15\r\nConnection: close\r\n\r\n404 Not Found\r\n";
static const char kResp503Sd[] PROGMEM =
  "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\n"
  "Content-Length: 23\r\nConnection: close\r\n\r\nSD card not available\r\n";
static const char kResp503Busy[] PROGMEM =
  "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\n"
  "Retry-After: 2\r\nContent-Length: 13\r\nConnection: close\r\n\r\nServer busy\r\n";

// Closed at once if it cannot be sent
static void sendFileError(EthernetClient &client, PGM_P resp, uint8_t* buf, size_t size) {
  ClientBufferPrint out(buf, size);
  out.print((const __FlashStringHelper*)resp);
  if (!sendBlock(client, out.data(), out.length(), SD_SEND_BLOCK_MS)) NetUtils::closeNow(client);
}

// True while c still holds the connection with that peer (any TCP state up
// to the close). Otherwise it ended, or the index was taken by someone else.
static bool peerIs(EthernetClient &c, uint16_t rport, uint32_t rip) {
  uint8_t sr = c.status();
  if (sr < SnSR::SYNRECV || sr > SnSR::LAST_ACK) return false;
  return c.localPort() == HTTP_PORT && c.remotePort() == rport &&
         (uint32_t)c.remoteIP() == rip;
}

static bool xferPeer(const Xfer &x, EthernetClient &c) {
  return peerIs(c, x.rport, x.rip);
}

// x's FIN is out: its socket goes to the linger table and the slot is
// freed. A full table closes its oldest socket at once.
static void lingerAdd(Xfer &x) {
  uint16_t now = (uint16_t)millis();
  Linger* l = &s_linger[0];
  for (uint8_t i = 0; i < SD_LINGERS; i++) {
    Linger &e = s_linger[i];
    if (!e.rport) { l = &e; break; }
    if ((uint16_t)(now - e.at) > (uint16_t)(now - l->at)) l = &e;
  }
  if (l->rport) {
    EthernetClient old(l->sock);
    if (peerIs(old, l->rport, l->rip)) NetUtils::closeNow(old);
  }
  l->sock = x.sock;
  l->rport = x.rport;
  l->rip = x.rip;
  l->at = now;
  x.state = XF_FREE;   // the file is already closed
}

// Forgets the sockets that closed (or were reused) and closes the ones
// still open after SD_SEND_LINGER_MS
static void lingerStep() {
  for (uint8_t i = 0; i < SD_LINGERS; i++) {
    Linger &l = s_linger[i];
    if (!l.rport) continue;
    EthernetClient c(l.sock);
    if (!peerIs(c, l.rport, l.rip)) {
      l.rport = 0;
    } else if ((uint16_t)((uint16_t)millis() - l.at) >= SD_SEND_LINGER_MS) {
      NetUtils::closeNow(c);
      l.rport = 0;
    }
  }
}

// Body done or cut: FIN once the queued data is out
static void xferFinish(Xfer &x) {
  x.f.close();
  x.state = XF_FIN;
  x.end = NetUtils::deadlineIn(SD_SEND_FIN_MS);
}

// Slot freed; the socket is not touched
static void xferDrop(Xfer &x) {
  x.f.close();
  x.state = XF_FREE;
}

static void xferCloseNow(Xfer &x, EthernetClient &c) {
  NetUtils::closeNow(c);
  xferDrop(x);
}

// Cuts the transfers reading 'name' (any case, as FAT), or all for NULL.
// A name that cannot be read back counts as a match.
static void abortTransfers(const char* name) {
  for (uint8_t i = 0; i < SD_XFERS; i++) {
    Xfer &x = s_xfer[i];
    if (x.state != XF_BODY) continue;
    if (name) {
      char n[WEB_NAME_MAX + 5];   // longest servable name plus ".gz"
      if (x.f.getName(n, sizeof(n)) > 0 && strcasecmp(n, name) != 0) continue;
    }
    Serial.println(F("Transfer cut: file changed"));
    xferFinish(x);
  }
}

// One step: queues what fits now. True if bytes were queued and more remain.
static bool xferStep(Xfer &x, uint8_t* buf) {
  EthernetClient c(x.sock);
  if (!xferPeer(x, c)) {
    xferDrop(x);   // reset or timed out: the index is not ours any more
    return false;
  }

  if (x.state == XF_FIN) {
    NetUtils::drainInput(c, 256, 0);   // unread input would turn the close into a reset
    int8_t r = NetUtils::sendFin(c);
    if (r > 0) {
      lingerAdd(x);
    } else if (r < 0 || NetUtils::expired(x.end)) {
      xferCloseNow(x, c);   // the data never got out: no FIN possible
    }
    return false;
  }

  // XF_BODY. The stall counts only while there is no room: after a pause
  // of the server (a long upload) a reader that kept reading goes on.
  int16_t room = NetUtils::txRoom(c);
  if (room < 0) {
    xferCloseNow(x, c);
    return false;
  }
  if (NetUtils::expired(x.end) ||
      (room == 0 && (uint16_t)((uint16_t)millis() - x.progressAt) >= SD_SEND_STALL_MS)) {
    Serial.println(F("Transfer cut: reader too slow"));
    xferFinish(x);
    return false;
  }
  if (room == 0) return false;   // SEND in flight or the reader is behind

  size_t n = (size_t)room;
  if (n > SD_BUFFER_SIZE) n = SD_BUFFER_SIZE;
  if (n > x.left) n = (size_t)x.left;
  if (x.f.read(buf, n) != (int)n) {   // read error or file shorter than its size
    cardFailed();
    Serial.println(F("Transfer cut: SD read error"));
    xferFinish(x);
    return false;
  }
  bool dead;
  if (!NetUtils::tryWriteAll(c, buf, n, &dead)) {
    if (dead) xferCloseNow(x, c);
    else x.f.seekSet(x.f.curPosition() - n);   // not taken: read again next time
    return false;
  }
  x.left -= n;
  x.progressAt = (uint16_t)millis();
  if (x.left == 0) {
    xferFinish(x);
    return false;
  }
  return true;
}

void SdUtils::pumpTransfers() {
  lingerStep();
  uint32_t start = millis();
  for (;;) {
    bool more = false;
    for (uint8_t i = 0; i < SD_XFERS; i++) {
      if (s_xfer[i].state != XF_FREE && xferStep(s_xfer[i], _sdBuffer)) more = true;
    }
    if (!more || (uint32_t)millis() - start >= SD_XFER_PASS_MS) return;
    wdt_reset();
  }
}

bool SdUtils::transferOwns(uint8_t sock) {
  for (uint8_t i = 0; i < SD_XFERS; i++) {
    const Xfer &x = s_xfer[i];
    if ((x.state == XF_BODY || x.state == XF_FIN) && x.sock == sock) return true;
  }
  return false;
}

bool SdUtils::transferSlotFree() {
  for (uint8_t i = 0; i < SD_XFERS; i++) {
    if (s_xfer[i].state == XF_FREE) return true;
  }
  return false;
}

bool SdUtils::serveFile(EthernetClient &client, const char* filename, bool acceptsGzip,
                        bool attachment) {
  Xfer* x = NULL;
  for (uint8_t i = 0; i < SD_XFERS && !x; i++) {
    if (s_xfer[i].state == XF_FREE) x = &s_xfer[i];
  }
  if (!x) {
    sendFileError(client, kResp503Busy, _sdBuffer, SD_BUFFER_SIZE);
    return false;
  }

  File &f = x->f;   // closed while FREE
  bool gzip = false;
  if (acceptsGzip && !attachment && strlen(filename) + 4 <= SD_LINE_BUFFER_SIZE) {
    strcpy(_lineBuffer, filename);
    strcat_P(_lineBuffer, PSTR(".gz"));
    gzip = openRead(f, _lineBuffer) && f.isFile();
    if (!gzip) f.close();
  }
  if (!gzip && !(openRead(f, filename) && f.isFile())) {
    f.close();
    sendFileError(client, s_sdOk ? kResp404 : kResp503Sd, _sdBuffer, SD_BUFFER_SIZE);
    return false;
  }

  uint32_t size = (uint32_t)f.fileSize();

  // Headers, and the first data in the rest of the same block
  size_t used;
  {
    ClientBufferPrint hdr(_sdBuffer, SD_BUFFER_SIZE);
    if (attachment) {
      hdr.print(F("HTTP/1.1 200 OK\r\nContent-Disposition: attachment; filename=\""));
      hdr.print(filename);
      hdr.print(F("\"\r\nContent-Type: application/octet-stream"));
    } else {
      hdr.print(F("HTTP/1.1 200 OK\r\nContent-Type: "));
      hdr.print((const __FlashStringHelper*)contentType(filename));
      if (gzip) hdr.print(F("\r\nContent-Encoding: gzip"));
    }
    hdr.print(F("\r\nContent-Length: "));
    hdr.print(size);
    hdr.print(F("\r\nConnection: close\r\n\r\n"));
    used = hdr.failed() ? SD_BUFFER_SIZE : hdr.length();
  }
  size_t first = SD_BUFFER_SIZE - used;
  if (first > size) first = (size_t)size;
  if (first > 0 && f.read(_sdBuffer + used, first) != (int)first) {
    // Nothing sent yet: 503
    f.close();
    cardFailed();
    sendFileError(client, kResp503Sd, _sdBuffer, SD_BUFFER_SIZE);
    return false;
  }
  // A fresh connection: the TX buffer is empty, this goes at once
  if (used >= SD_BUFFER_SIZE || !sendBlock(client, _sdBuffer, used + first, SD_SEND_BLOCK_MS)) {
    f.close();
    NetUtils::closeNow(client);
    return false;
  }

  NetUtils::drainInput(client, 2048, 0);   // the rest of the request
  x->sock = client.getSocketNumber();
  x->rport = client.remotePort();
  x->rip = (uint32_t)client.remoteIP();
  x->left = size - first;
  uint32_t budget = SD_SEND_BASE_MS + size / SD_SEND_BYTES_PER_MS;
  if (budget > SD_SEND_MAX_MS) budget = SD_SEND_MAX_MS;
  x->end = NetUtils::deadlineIn(budget);
  x->progressAt = (uint16_t)millis();
  x->state = XF_BODY;
  if (x->left == 0) xferFinish(*x);
  client = EthernetClient();   // the transfer owns the socket now
  return true;
}
