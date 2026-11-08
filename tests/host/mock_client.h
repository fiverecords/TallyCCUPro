// mock_client.h - EthernetClient whose input is scripted, for the NetUtils
// readers. Input is a list of segments. add() queues a segment that has
// already arrived; addLater() queues one that only becomes readable after
// arrive(), as TCP segments landing between two polls.
#ifndef HOST_MOCK_CLIENT_H
#define HOST_MOCK_CLIENT_H

#include <Arduino.h>
#include <Ethernet.h>

class MockClient : public EthernetClient {
 public:
  enum { MAX_SEGMENTS = 16 };

  MockClient() : _count(0), _arrived(0), _cur(0), _pos(0), _connected(true), _reads(0) {}

  // Appends a segment (the pointer must stay valid).
  void add(const char* data) { addLater(data); arrive(); }
  void addLater(const char* data) { addLater(data, strlen(data)); }
  void addLater(const char* data, size_t len) {
    if (_count < MAX_SEGMENTS) {
      _seg[_count] = data;
      _len[_count] = len;
      _count++;
    }
  }
  // The next held segment becomes readable.
  void arrive() { if (_arrived < _count) _arrived++; }
  void setConnected(bool c) { _connected = c; }
  // Bytes consumed so far (read() calls that returned data)
  int reads() const { return _reads; }
  size_t left() const {
    size_t n = 0;
    for (int i = _cur; i < _count; i++) n += _len[i] - (i == _cur ? _pos : 0);
    return n;
  }

  virtual int available() {
    skipEmpty();
    int n = 0;
    for (int i = _cur; i < _arrived; i++) n += (int)(_len[i] - (i == _cur ? _pos : 0));
    return n;
  }
  virtual int read() {
    skipEmpty();
    if (_cur >= _arrived) return -1;
    _reads++;
    return (uint8_t)_seg[_cur][_pos++];
  }
  virtual int read(uint8_t* buf, size_t size) {
    int n = 0;
    while ((size_t)n < size && available() > 0) {
      int c = read();
      if (buf) buf[n] = (uint8_t)c;
      n++;
    }
    return n > 0 ? n : -1;
  }
  virtual int peek() {
    skipEmpty();
    return _cur < _arrived ? (uint8_t)_seg[_cur][_pos] : -1;
  }
  virtual uint8_t connected() { return _connected || available() > 0; }
  virtual size_t write(uint8_t) { return 0; }
  virtual size_t write(const uint8_t*, size_t) { return 0; }
  virtual void flush() {}
  virtual void stop() { _connected = false; }

 private:
  void skipEmpty() {
    while (_cur < _arrived && _pos >= _len[_cur]) {
      _cur++;
      _pos = 0;
    }
  }
  const char* _seg[MAX_SEGMENTS];
  size_t _len[MAX_SEGMENTS];
  int _count, _arrived, _cur;
  size_t _pos;
  bool _connected;
  int _reads;
};

#endif  // HOST_MOCK_CLIENT_H
