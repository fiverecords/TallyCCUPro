/**
 * NetUtils.cpp - Bounded, watchdog-safe network I/O helpers
 *
 * The TX path talks to the W5x00 registers directly (same steps as the
 * library's socketSend) so that SEND_OK is never waited for without limit.
 */

#include "NetUtils.h"
#include <SPI.h>
#include <utility/w5100.h>
#include <avr/wdt.h>

#if MAX_SOCK_NUM > 8
#error "NetUtils: in-flight mask holds 8 sockets"
#endif

namespace {

const uint16_t SETTLE_US = 300;   // SEND_OK grace after SEND (healthy links finish in tens of us)
const uint16_t MIN_CHUNK = 256;   // writeWithDeadline: wait for this much room (or the rest)
const uint16_t FIN_MIN_MS = 10;   // closeGracefully: minimum wait after FIN
const size_t POLL_BYTES = 256;    // readLine: bytes per LineReader::poll() between checks

// One bit per socket: a NetUtils SEND may still be in flight.
uint8_t s_inflight;

inline bool canSend(uint8_t sr) {
  return sr == SnSR::ESTABLISHED || sr == SnSR::CLOSE_WAIT;
}

// TCP connection states, SYNRECV..LAST_ACK (0x19 is unused). Not LISTEN,
// INIT, SYNSENT, CLOSED or UDP/IPRAW/MACRAW: those are someone else's.
inline bool inConnection(uint8_t sr) {
  return sr >= SnSR::SYNRECV && sr <= SnSR::LAST_ACK;
}

inline uint32_t since(uint32_t start) {
  return (uint32_t)millis() - start;
}

// 16-bit chip counters can change between their two bytes: read until two
// reads agree, a few times at most. Caller holds the SPI transaction.
bool stableFSR(uint8_t s, uint16_t* out) {
  uint16_t prev = W5100.readSnTX_FSR(s);
  for (uint8_t i = 0; i < 4; i++) {
    uint16_t v = W5100.readSnTX_FSR(s);
    if (v == prev) { *out = v; return true; }
    prev = v;
  }
  return false;
}

bool stableTXRD(uint8_t s, uint16_t* out) {
  uint16_t prev = W5100.readSnTX_RD(s);
  for (uint8_t i = 0; i < 4; i++) {
    uint16_t v = W5100.readSnTX_RD(s);
    if (v == prev) { *out = v; return true; }
    prev = v;
  }
  return false;
}

bool stableRSR(uint8_t s, uint16_t* out) {
  uint16_t prev = W5100.readSnRX_RSR(s);
  for (uint8_t i = 0; i < 4; i++) {
    uint16_t v = W5100.readSnRX_RSR(s);
    if (v == prev) { *out = v; return true; }
    prev = v;
  }
  return false;
}

// No SEND of ours in flight. Caller holds the SPI transaction.
bool idleLocked(uint8_t s) {
  uint8_t bit = (uint8_t)(1 << s);
  if (!(s_inflight & bit)) return true;
  if (W5100.readSnIR(s) & SnIR::SEND_OK) {
    W5100.writeSnIR(s, SnIR::SEND_OK);
    s_inflight &= (uint8_t)~bit;
    return true;
  }
  // Sn_TX_RD reaches Sn_TX_WR once SEND has transmitted everything
  uint16_t rd;
  if (stableTXRD(s, &rd) && rd == W5100.readSnTX_WR(s)) {
    s_inflight &= (uint8_t)~bit;
    return true;
  }
  return false;
}

// Copies len bytes at Sn_TX_WR and issues SEND (as the library's write_data
// + socketSend), then waits SEND_OK for SETTLE_US only. Holds SPI.
void queueLocked(uint8_t s, const uint8_t* buf, uint16_t len) {
  uint16_t ptr = W5100.readSnTX_WR(s);
  uint16_t offset = ptr & W5100.SMASK;
  uint16_t dst = offset + W5100.SBASE(s);
  if (W5100.hasOffsetAddressMapping() || offset + len <= W5100.SSIZE) {
    W5100.write(dst, buf, len);
  } else {
    uint16_t first = W5100.SSIZE - offset;
    W5100.write(dst, buf, first);
    W5100.write(W5100.SBASE(s), buf + first, len - first);
  }
  W5100.writeSnTX_WR(s, (uint16_t)(ptr + len));
  W5100.writeSnIR(s, SnIR::SEND_OK);  // any SEND_OK from here on is ours
  W5100.execCmdSn(s, Sock_SEND);
  uint8_t bit = (uint8_t)(1 << s);
  s_inflight |= bit;
  uint16_t t0 = (uint16_t)micros();
  do {
    if (W5100.readSnIR(s) & SnIR::SEND_OK) {
      W5100.writeSnIR(s, SnIR::SEND_OK);
      s_inflight &= (uint8_t)~bit;
      return;
    }
  } while ((uint16_t)((uint16_t)micros() - t0) < SETTLE_US);
}

// Queues up to len bytes if the socket is idle and at least
// min(len, minChunk) bytes fit. Returns bytes queued (0 = busy or no room),
// -1 if the connection can no longer send.
int16_t queueSome(uint8_t s, const uint8_t* buf, size_t len, size_t minChunk) {
  int16_t ret = 0;
  SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
  if (!canSend(W5100.readSnSR(s))) {
    s_inflight &= (uint8_t)~(1 << s);
    ret = -1;
  } else if (idleLocked(s)) {
    uint16_t fsr;
    if (stableFSR(s, &fsr)) {
      size_t n = len < fsr ? len : fsr;
      if (n > W5100.SSIZE) n = W5100.SSIZE;
      size_t want = len < minChunk ? len : minChunk;
      if (n > 0 && n >= want) {
        queueLocked(s, buf, (uint16_t)n);
        ret = (int16_t)n;
      }
    }
  }
  SPI.endTransaction();
  return ret;
}

// Caller holds the SPI transaction (both)
void readPeerLocked(uint8_t s, NetUtils::Peer* p) {
  W5100.readSnDIPR(s, (uint8_t*)&p->ip);
  p->port = W5100.readSnDPORT(s);
  p->lport = W5100.readSnPORT(s);
}

// A LISTEN keeps the old peer registers: hence the state check
bool samePeerLocked(uint8_t s, uint8_t sr, const NetUtils::Peer& p) {
  if (!inConnection(sr)) return false;
  NetUtils::Peer q;
  readPeerLocked(s, &q);
  return q.ip == p.ip && q.port == p.port && q.lport == p.lport;
}

// The index may already belong to a new LISTEN or connection: close only a
// TCP connection, and release the client without stop() (its DISCON and
// 1 s wait would hit whatever now uses the index).
void forceClose(EthernetClient& c, uint8_t s) {
  SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
  if (inConnection(W5100.readSnSR(s))) W5100.execCmdSn(s, Sock_CLOSE);
  SPI.endTransaction();
  s_inflight &= (uint8_t)~(1 << s);
  c = EthernetClient();
}

}  // namespace

namespace NetUtils {

bool tryWriteAll(EthernetClient& c, const uint8_t* buf, size_t len, bool* dead) {
  int16_t r;
  if (c.getSocketNumber() >= MAX_SOCK_NUM) r = -1;
  else if (len == 0) r = 0;
  else if (len > W5100.SSIZE) r = sendable(c) ? 0 : -1;
  else r = queueSome(c.getSocketNumber(), buf, len, len);
  if (dead) *dead = (r < 0);
  return r >= 0 && (size_t)r == len;
}

bool sendable(EthernetClient& c) {
  uint8_t s = c.getSocketNumber();
  if (s >= MAX_SOCK_NUM) return false;
  SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
  bool ok = canSend(W5100.readSnSR(s));
  SPI.endTransaction();
  return ok;
}

size_t writeWithDeadline(EthernetClient& c, const uint8_t* buf, size_t len,
                         unsigned long timeoutMs) {
  uint8_t s = c.getSocketNumber();
  if (s >= MAX_SOCK_NUM) return 0;
  uint32_t start = millis();
  size_t done = 0;
  while (done < len) {
    wdt_reset();
    int16_t r = queueSome(s, buf + done, len - done, MIN_CHUNK);
    if (r < 0) break;
    done += (size_t)r;
    if (done >= len || since(start) >= (uint32_t)timeoutMs) break;
  }
  return done;
}

bool txIdle(EthernetClient& c) {
  uint8_t s = c.getSocketNumber();
  if (s >= MAX_SOCK_NUM) return false;
  SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
  bool idle = idleLocked(s);
  SPI.endTransaction();
  return idle;
}

int16_t txRoom(EthernetClient& c) {
  uint8_t s = c.getSocketNumber();
  if (s >= MAX_SOCK_NUM) return -1;
  int16_t ret = 0;
  SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
  if (!canSend(W5100.readSnSR(s))) {
    s_inflight &= (uint8_t)~(1 << s);
    ret = -1;
  } else if (idleLocked(s)) {
    uint16_t fsr;
    if (stableFSR(s, &fsr)) ret = (int16_t)(fsr < W5100.SSIZE ? fsr : W5100.SSIZE);
  }
  SPI.endTransaction();
  return ret;
}

int8_t sendFin(EthernetClient& c) {
  uint8_t s = c.getSocketNumber();
  if (s >= MAX_SOCK_NUM) return -1;
  int8_t ret = 0;
  SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
  if (!canSend(W5100.readSnSR(s))) {
    s_inflight &= (uint8_t)~(1 << s);
    ret = -1;
  } else if (idleLocked(s)) {
    W5100.execCmdSn(s, Sock_DISCON);   // FIN behind data already on the wire
    ret = 1;
  }
  SPI.endTransaction();
  return ret;
}

size_t peek(EthernetClient& c, uint8_t* buf, size_t len) {
  uint8_t s = c.getSocketNumber();
  if (s >= MAX_SOCK_NUM || len == 0) return 0;
  size_t n = 0;
  SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
  uint16_t rsr;
  if (stableRSR(s, &rsr) && rsr > 0) {
    n = rsr < len ? rsr : len;
    uint16_t off = W5100.readSnRX_RD(s) & W5100.SMASK;
    uint16_t src = W5100.RBASE(s) + off;
    if (W5100.hasOffsetAddressMapping() || off + n <= W5100.SSIZE) {
      W5100.read(src, buf, (uint16_t)n);
    } else {
      uint16_t first = W5100.SSIZE - off;
      W5100.read(src, buf, first);
      W5100.read(W5100.RBASE(s), buf + first, (uint16_t)(n - first));
    }
  }
  SPI.endTransaction();
  return n;
}

// poll() may re-arm a listener on the index once the connection is gone,
// and a client may land there: the socket is only touched while it still
// holds the connection it had on entry.
void closeGracefully(EthernetClient& c, unsigned long timeoutMs, void (*poll)()) {
  uint8_t s = c.getSocketNumber();
  if (s >= MAX_SOCK_NUM) return;
  uint32_t start = millis();
  Peer peer;
  SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
  readPeerLocked(s, &peer);
  SPI.endTransaction();
  bool fin = false;
  for (bool first = true;; first = false) {
    wdt_reset();
    if (poll && !first) poll();
    // Once SEND is done the data is on the wire: FIN goes behind it and
    // unACKed data is still retransmitted in FIN_WAIT. No delayed-ACK wait.
    SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
    uint8_t sr = W5100.readSnSR(s);
    bool mine = samePeerLocked(s, sr, peer);
    bool ok = mine && canSend(sr) && idleLocked(s);
    if (ok) W5100.execCmdSn(s, Sock_DISCON);
    SPI.endTransaction();
    if (ok) { fin = true; break; }
    if (!mine) { c = EthernetClient(); return; }   // closed, or no longer ours
    if (!canSend(sr) || since(start) >= (uint32_t)timeoutMs) break;
  }
  if (fin) {
    uint32_t used = since(start);
    uint32_t finWait = (uint32_t)timeoutMs > used ? (uint32_t)timeoutMs - used : 0;
    if (finWait < FIN_MIN_MS) finWait = FIN_MIN_MS;
    uint32_t t0 = millis();
    for (;;) {
      wdt_reset();
      SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
      bool mine = samePeerLocked(s, W5100.readSnSR(s), peer);
      SPI.endTransaction();
      if (!mine) { c = EthernetClient(); return; }   // closed, or no longer ours
      if (since(t0) >= finWait) break;
      if (poll) poll();
    }
  }
  forceClose(c, s);   // still ours: no poll() since the last check
}

void closeNow(EthernetClient& c) {
  uint8_t s = c.getSocketNumber();
  if (s >= MAX_SOCK_NUM) return;
  forceClose(c, s);
}

void peerOf(EthernetClient& c, Peer* p) {
  memset(p, 0, sizeof(*p));
  uint8_t s = c.getSocketNumber();
  if (s >= MAX_SOCK_NUM) return;
  SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
  readPeerLocked(s, p);
  SPI.endTransaction();
}

bool samePeer(EthernetClient& c, const Peer& p) {
  uint8_t s = c.getSocketNumber();
  if (s >= MAX_SOCK_NUM) return false;
  SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
  bool same = samePeerLocked(s, W5100.readSnSR(s), p);
  SPI.endTransaction();
  return same;
}

void setRetransmission(uint16_t timeoutMs, uint8_t count) {
  uint16_t rtr = timeoutMs > 6553 ? 0xFFFF : (uint16_t)(timeoutMs * 10);  // 100 us units
  SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
  if (W5100.getChip() == 55) {
    uint8_t b[2] = {(uint8_t)(rtr >> 8), (uint8_t)rtr};
    W5100.write(0x0019, b, 2);   // W5500 RTR
    W5100.write(0x001B, count);  // W5500 RCR
  } else {
    W5100.writeRTR(rtr);  // W5100/W5200 map
    W5100.writeRCR(count);
  }
  SPI.endTransaction();
}

void readRetransmission(uint16_t* timeoutMs, uint8_t* count) {
  uint8_t b[2];
  SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
  if (W5100.getChip() == 55) {
    W5100.read(0x0019, b, 2);
    *count = W5100.read(0x001B);
  } else {
    uint16_t r = W5100.readRTR();
    b[0] = (uint8_t)(r >> 8);
    b[1] = (uint8_t)r;
    *count = W5100.readRCR();
  }
  SPI.endTransaction();
  *timeoutMs = (uint16_t)((((uint16_t)b[0] << 8) | b[1]) / 10);
}

int readLine(EthernetClient& c, char* buf, size_t size, unsigned long deadline,
             bool* truncated, long* remaining) {
  LineReader lr(buf, size);
  if (truncated) *truncated = false;
  for (;;) {
    wdt_reset();
    if (remaining && *remaining <= 0) return -1;
    int n = lr.poll(c, POLL_BYTES, remaining);
    if (n >= 0) {
      if (truncated) *truncated = lr.truncated();
      return n;
    }
    if (c.available() == 0 && !c.connected()) return -1;
    if (expired(deadline)) return -1;
  }
}

long drainInput(EthernetClient& c, long maxBytes, unsigned long timeoutMs) {
  long done = 0;
  uint32_t start = millis();
  while (done < maxBytes) {
    wdt_reset();
    int n = c.available();
    if (n > 0) {
      if ((long)n > maxBytes - done) n = (int)(maxBytes - done);
      int r = c.read((uint8_t*)NULL, (size_t)n);  // NULL: skip without copying
      if (r <= 0) break;
      done += r;
    } else if (!c.connected()) {
      break;
    }
    if (since(start) >= (uint32_t)timeoutMs) break;
  }
  return done;
}

LineReader::LineReader(char* buf, size_t size)
    : _buf(buf), _size(size), _len(0), _flags(0) {
  if (_size) _buf[0] = '\0';
}

void LineReader::reset() {
  _len = 0;
  _flags = 0;
  if (_size) _buf[0] = '\0';
}

int LineReader::poll(EthernetClient& c, size_t maxBytes, long* remaining) {
  if (_flags & F_DONE) reset();
  if (remaining && *remaining <= 0) return -1;
  int avail = c.available();
  while (avail > 0 && maxBytes > 0) {
    int ch = c.read();
    if (ch < 0) break;
    avail--;
    maxBytes--;
    bool end = (ch == '\n');
    if (remaining && --(*remaining) <= 0) end = true;
    if (ch != '\n' && ch != '\r') {
      if (_len + 1 < _size) _buf[_len++] = (char)ch;
      else _flags |= F_TRUNC;
    }
    if (end) {
      if (_size) _buf[_len] = '\0';
      _flags |= F_DONE;
      return (int)_len;
    }
    if (avail == 0) avail = c.available();
  }
  if (_size) _buf[_len] = '\0';
  return -1;
}

}  // namespace NetUtils

ClientBufferPrint::ClientBufferPrint(EthernetClient& c, uint8_t* buf, size_t size,
                                     unsigned long timeoutMs, void (*poll)())
    : _c(&c), _poll(poll), _buf(buf), _size(size), _len(0),
      _deadline(NetUtils::deadlineIn(timeoutMs)), _failed(false) {}

ClientBufferPrint::ClientBufferPrint(uint8_t* buf, size_t size)
    : _c(NULL), _poll(NULL), _buf(buf), _size(size), _len(0), _deadline(0),
      _failed(false) {}

void ClientBufferPrint::fail() {
  _failed = true;
  setWriteError();
}

// Within the object's budget; with _poll, in slices with _poll() between
size_t ClientBufferPrint::send(const uint8_t* data, size_t len) {
  size_t done = 0;
  for (;;) {
    int32_t t = (int32_t)((uint32_t)_deadline - (uint32_t)millis());
    unsigned long left = t > 0 ? (unsigned long)t : 0;
    unsigned long slice = (_poll && left > NETUTILS_POLL_SLICE_MS) ? NETUTILS_POLL_SLICE_MS : left;
    done += NetUtils::writeWithDeadline(*_c, data + done, len - done, slice);
    if (done >= len || slice >= left || !NetUtils::sendable(*_c)) return done;
    _poll();
  }
}

size_t ClientBufferPrint::write(uint8_t b) {
  return write(&b, 1);
}

size_t ClientBufferPrint::write(const uint8_t* data, size_t len) {
  size_t done = 0;
  while (done < len && !_failed) {
    size_t left = len - done;
    if (_c && _len == 0 && left >= _size) {
      // Block at least as large as the buffer: send it without copying
      size_t w = send(data + done, left);
      done += w;
      if (w != left) fail();
      break;
    }
    size_t room = _size - _len;
    if (room == 0) {
      if (!_c) { fail(); break; }  // compose mode overflow
      flush();
      continue;
    }
    size_t n = left < room ? left : room;
    memcpy(_buf + _len, data + done, n);
    _len += n;
    done += n;
  }
  return done;
}

int ClientBufferPrint::availableForWrite() {
  return (int)(_size - _len);
}

void ClientBufferPrint::flush() {
  if (!_c) return;
  if (!_failed && _len > 0) {
    if (send(_buf, _len) != _len) fail();
  }
  _len = 0;
}
