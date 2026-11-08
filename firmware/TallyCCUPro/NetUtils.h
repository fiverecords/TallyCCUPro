/**
 * NetUtils.h - Bounded, watchdog-safe network I/O helpers for EthernetClient
 *
 * EthernetClient::write() waits without limit for room in the socket TX
 * buffer and then for SEND_OK (a peer with a zero window never gives it),
 * flush() waits without limit for the TX buffer to drain, and none of them
 * feed the watchdog. Checking availableForWrite() before write() does not
 * help: write() still waits for SEND_OK. These helpers never wait past their
 * budget, call wdt_reset() in every loop and are millis() overflow safe.
 *
 * Send model (W5x00): data is copied into the chip and SEND is issued, but
 * SEND_OK is only waited for ~0.3 ms. If it has not come yet the SEND stays
 * in flight and the next call finds the socket busy instead of blocking.
 * It is done once SEND_OK arrives or Sn_TX_RD reaches Sn_TX_WR. NetUtils
 * never issues a SEND while one of its own is still in flight.
 *
 * Rules for callers:
 *  - A failed write (short count, false, failed()) leaves the stream
 *    incomplete: close the client (closeGracefully() or closeNow()).
 *  - Do not mix EthernetClient::write()/print() and NetUtils writes on the
 *    same socket unless txIdle() is true.
 *  - Deadlines are absolute millis() values valid for spans < 24.8 days.
 *  - An EthernetClient is only a socket index. Once its connection ends the
 *    library hands that index to the next one (a LISTEN of any server, an
 *    HTTP request, Companion...), possibly within one loop() pass.
 *    Long-lived clients (SSE, 8098) must store localPort(), remoteIP() and
 *    remotePort() at accept (peerOf()) and compare them before each use
 *    (read, write, close; samePeer()); on a mismatch drop the client with
 *    'c = EthernetClient();'
 *    (never stop() or closeNow(): they would hit the new owner). A dead
 *    socket (tryWriteAll() *dead, sendable() false) is forgotten at once,
 *    the same way or with closeNow(), which only closes TCP connections.
 *
 * Static RAM: 1 byte (in-flight mask); buffers are provided by the caller.
 * Depends on Ethernet 2.x internals (getSocketNumber(), utility/w5100.h).
 */

#ifndef NETUTILS_H
#define NETUTILS_H

#include <Arduino.h>
#include <Ethernet.h>

namespace NetUtils {

// True once the absolute millis() instant 'deadline' has been reached.
inline bool expired(unsigned long deadline) {
  return (int32_t)((uint32_t)millis() - (uint32_t)deadline) >= 0;
}

// Absolute deadline timeoutMs from now, for readLine().
inline unsigned long deadlineIn(unsigned long timeoutMs) {
  return (unsigned long)((uint32_t)millis() + (uint32_t)timeoutMs);
}

// Queues all len bytes if they fit in the socket TX buffer right now and no
// SEND is in flight; otherwise queues nothing. Never waits for the peer
// (at most ~0.3 ms for SEND_OK). For SSE events and 8098 lines.
// len == 0 returns true (false for a released client). len > TX buffer size
// (2 KB) always returns false. dead (optional) tells a socket that can no
// longer send (not ESTABLISHED or CLOSE_WAIT, or a released client) from a
// full or busy one: forget the client on the first dead, keep it (and count
// drops) otherwise.
bool tryWriteAll(EthernetClient& c, const uint8_t* buf, size_t len,
                 bool* dead = NULL);

// True while the socket can still send (ESTABLISHED or CLOSE_WAIT). In
// CLOSE_WAIT the peer has closed its side: a long-lived client there is gone.
bool sendable(EthernetClient& c);

// Writes len bytes, waiting for room at most timeoutMs in total (0 = a
// single attempt). Returns the bytes queued: len on success, less if the
// budget ran out or the connection closed (then close the client).
size_t writeWithDeadline(EthernetClient& c, const uint8_t* buf, size_t len,
                         unsigned long timeoutMs);

// True when no NetUtils SEND is in flight on the socket (clears a pending
// SEND_OK). False for a closed client.
bool txIdle(EthernetClient& c);

// Bytes tryWriteAll() would queue right now: the free TX space (at most the
// TX buffer size) when no SEND is in flight, 0 while one is or the buffer
// is full, -1 if the connection can no longer send (or a released client).
// Never waits. For writers that must not take data they cannot send yet.
int16_t txRoom(EthernetClient& c);

// Non-blocking half of closeGracefully(): once nothing is in flight, sends
// FIN behind the queued data and returns 1 (the socket then closes on its
// own; the client is not released). 0 while a SEND is still in flight
// (call again later), -1 if the connection can no longer send.
int8_t sendFin(EthernetClient& c);

// Copies up to len received bytes without consuming them; returns the
// count. Reads the chip RX buffer at Sn_RX_RD, so only valid while nothing
// has been read from this connection yet (the library moves Sn_RX_RD late).
size_t peek(EthernetClient& c, uint8_t* buf, size_t len);

// Waits up to timeoutMs for everything queued to be transmitted (not ACKed:
// no delayed-ACK stall), then sends FIN behind the data and waits (within
// the same budget, at least 10 ms) for the close. If the data cannot be
// transmitted in time (zero window) or the close does not complete, the
// socket is closed at once. Replaces flush() + stop(). The client is
// released in every case. poll (optional) is called while waiting; it must
// not use this connection, but may reuse the index once the connection has
// closed (a listener): the socket is only touched while it still holds the
// connection (state and peer) it had on entry.
void closeGracefully(EthernetClient& c, unsigned long timeoutMs, void (*poll)() = NULL);

// Closes the socket at once, without FIN or waiting (dead or stuck peer).
// The peer gets an RST when it next sends something. Only TCP connection
// states are closed; a socket already CLOSED or reused as LISTEN, INIT,
// SYNSENT or UDP is left alone. The client is released (no stop()).
void closeNow(EthernetClient& c);

// Who a socket is connected to, for clients kept across code that polls
// the network (the index may be handed on meanwhile, see the rules above).
struct Peer {
  uint32_t ip;
  uint16_t port, lport;
};
// Reads c's peer and local port (all 0 for a released client)
void peerOf(EthernetClient& c, Peer* p);
// True while c still holds a TCP connection (SYNRECV..LAST_ACK) with p
bool samePeer(EthernetClient& c, const Peer& p);

// Sets the chip-wide TCP retransmission timeout and retry count (all
// sockets, vMix included). A peer that stops ACKing is dropped after about
// timeoutMs * (2^(count+1) - 1), e.g. 100 ms x 4 -> 3.1 s. Writes the W5500
// registers (0x19/0x1B); Ethernet.setRetransmission*() writes the W5100
// addresses, which on a W5500 land on SIR/SIMR/RTR. timeoutMs <= 6553.
void setRetransmission(uint16_t timeoutMs, uint8_t count);
// Reads back what the chip holds (for a boot log).
void readRetransmission(uint16_t* timeoutMs, uint8_t* count);

// Reads one line ending in '\n'. Every '\r' is dropped; buf is always
// NUL-terminated (size >= 1). Waits until the absolute millis() 'deadline'.
// Returns the line length, or -1 on deadline, disconnect, or when
// *remaining is already <= 0. A partial line is then lost but its tail is
// still in the stream: after -1, close the connection. For request/response
// only (HTTP); readers that keep the connection (8098) use LineReader.
// *truncated (optional) is set when the line did not fit: the stored prefix
// is returned and the rest of the line is consumed up to '\n'.
// remaining (optional): bytes left in a body (Content-Length); each byte
// read decrements it and reaching 0 ends the line even without '\n'.
int readLine(EthernetClient& c, char* buf, size_t size, unsigned long deadline,
             bool* truncated, long* remaining = NULL);

// Discards input until maxBytes are dropped, the peer has closed and nothing
// is left, or timeoutMs has passed (0 = drop only what has already arrived).
// Returns the number of bytes discarded.
long drainInput(EthernetClient& c, long maxBytes, unsigned long timeoutMs);

// Incremental line assembler for non-blocking readers that keep partial
// lines across loop() passes (one per connection; caller owns buf).
// Same line rules as readLine(), but a partial line is kept, never lost.
class LineReader {
 public:
  LineReader(char* buf, size_t size);
  // Consumes at most maxBytes of data that has already arrived; never waits.
  // Returns the line length when a full line is in buf, -1 otherwise.
  // The next poll() after a complete line starts a new one.
  int poll(EthernetClient& c, size_t maxBytes, long* remaining = NULL);
  bool truncated() const { return _flags & F_TRUNC; }
  size_t length() const { return _len; }
  void reset();

 private:
  enum { F_TRUNC = 1, F_DONE = 2 };
  char* _buf;
  size_t _size;
  size_t _len;
  uint8_t _flags;
};

}  // namespace NetUtils

// Print with a caller-owned buffer (WDT-03): groups print()/print(F()) into
// block writes instead of one TCP segment per character.
//
// Stream mode, ClientBufferPrint(client, buf, size, timeoutMs[, poll]): the
// buffer is written with NetUtils::writeWithDeadline() when it fills and on
// flush(). timeoutMs is the total budget of the object, counted from
// construction. With poll, a write that has to wait for room waits in
// slices of NETUTILS_POLL_SLICE_MS and calls poll() between them (a stalled
// reader must not stop the tally); poll must not use buf.
// If a write runs out of time the object fails: the rest is discarded and
// writes return 0 (close the client). The destructor flushes, but call
// flush() yourself before closing the client.
//
// Compose mode, ClientBufferPrint(buf, size): no client, nothing is sent.
// The message is built in buf for an all-or-nothing send, e.g. one SSE event
// or 8098 line to several clients with NetUtils::tryWriteAll(c, data(),
// length()). Overflow sets failed().
#define NETUTILS_POLL_SLICE_MS 20

class ClientBufferPrint : public Print {
 public:
  ClientBufferPrint(EthernetClient& c, uint8_t* buf, size_t size,
                    unsigned long timeoutMs, void (*poll)() = NULL);
  ClientBufferPrint(uint8_t* buf, size_t size);
  ~ClientBufferPrint() { flush(); }
  ClientBufferPrint(const ClientBufferPrint&) = delete;
  ClientBufferPrint& operator=(const ClientBufferPrint&) = delete;

  virtual size_t write(uint8_t b);
  virtual size_t write(const uint8_t* data, size_t len);
  virtual int availableForWrite();
  virtual void flush();
  using Print::write;

  bool failed() const { return _failed; }
  const uint8_t* data() const { return _buf; }
  size_t length() const { return _len; }
  // Empties the buffer. In compose mode it also clears the failure (reuse);
  // in stream mode a failure stays: the response is already truncated.
  void clear() {
    _len = 0;
    if (!_c) { _failed = false; clearWriteError(); }
  }

 private:
  void fail();
  size_t send(const uint8_t* data, size_t len);
  EthernetClient* _c;
  void (*_poll)();
  uint8_t* _buf;
  size_t _size;
  size_t _len;
  unsigned long _deadline;
  bool _failed;
};

#endif  // NETUTILS_H
