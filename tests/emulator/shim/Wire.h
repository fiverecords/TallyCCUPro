/* Wire.h - HOST shim: I2C is not emulated (the BMD shield is emulated at the
 * BMDSDIControl API level). All transfers succeed and read nothing. */
#ifndef TwoWire_h
#define TwoWire_h
#include <inttypes.h>
#include <Arduino.h>
#include "Stream.h"
bool emu_shield_present();

#define BUFFER_LENGTH 32
#define WIRE_HAS_END 1
#define WIRE_HAS_TIMEOUT 1

class TwoWire : public Stream {
public:
  TwoWire() {}
  void begin() {}
  void begin(uint8_t address) { (void)address; }
  void begin(int address) { (void)address; }
  void end() {}
  void setClock(uint32_t clock) { (void)clock; }
  void setWireTimeout(uint32_t timeout = 25000, bool reset_with_timeout = false) { (void)timeout; (void)reset_with_timeout; }
  bool getWireTimeoutFlag(void) { return false; }
  void clearWireTimeoutFlag(void) {}
  void beginTransmission(uint8_t address) { (void)address; }
  void beginTransmission(int address) { (void)address; }
  uint8_t endTransmission(void) { return emu_shield_present() ? 0 : 2; }
  uint8_t endTransmission(uint8_t sendStop) { (void)sendStop; return emu_shield_present() ? 0 : 2; }
  uint8_t requestFrom(uint8_t a, uint8_t q) { (void)a; (void)q; return 0; }
  uint8_t requestFrom(uint8_t a, uint8_t q, uint8_t s) { (void)a; (void)q; (void)s; return 0; }
  uint8_t requestFrom(uint8_t a, uint8_t q, uint32_t i, uint8_t n, uint8_t s) { (void)a; (void)q; (void)i; (void)n; (void)s; return 0; }
  uint8_t requestFrom(int a, int q) { (void)a; (void)q; return 0; }
  uint8_t requestFrom(int a, int q, int s) { (void)a; (void)q; (void)s; return 0; }
  virtual size_t write(uint8_t b) { (void)b; return 1; }
  virtual size_t write(const uint8_t *buf, size_t n) { (void)buf; return n; }
  virtual int available(void) { return 0; }
  virtual int read(void) { return -1; }
  virtual int peek(void) { return -1; }
  virtual void flush(void) {}
  void onReceive(void (*cb)(int)) { (void)cb; }
  void onRequest(void (*cb)(void)) { (void)cb; }
  inline size_t write(unsigned long n) { return write((uint8_t)n); }
  inline size_t write(long n) { return write((uint8_t)n); }
  inline size_t write(unsigned int n) { return write((uint8_t)n); }
  inline size_t write(int n) { return write((uint8_t)n); }
  using Print::write;
};

extern TwoWire Wire;
#endif
