// BMDSDIControl.h - COMPILE-ONLY STUB of the Blackmagic 3G-SDI Shield
// library, for CI and local builds without the proprietary library.
//
// Signatures follow the calls TallyCCU Pro makes (approximated from the
// public shield manual). Array overloads are modelled as template<int N> on
// the array length; this is believed to match the real library but is not
// confirmed. Every call does a small I2C write so that code size and static
// RAM stay in the same range as with the real library (which is larger).
//
// A firmware built with this stub does NOT control cameras: never flash it.
#ifndef BMDSDICONTROL_STUB_H
#define BMDSDICONTROL_STUB_H

#include <Arduino.h>
#include <Wire.h>

#define BMDSDICONTROL_STUB 1

class BMD_SDIControl_I2C_Base {
 public:
  explicit BMD_SDIControl_I2C_Base(int addr) : _addr(addr) {}
  void begin() { Wire.begin(); }
  unsigned long getFirmwareVersion() const { return 0; }
  unsigned long getProtocolVersion() const { return 0; }

 protected:
  int _addr;
  void raw(const void* p, int n) const {
    Wire.beginTransmission(_addr);
    Wire.write((const uint8_t*)p, n);
    Wire.endTransmission();
  }
};

class BMD_SDITallyControl_I2C : public BMD_SDIControl_I2C_Base {
 public:
  using BMD_SDIControl_I2C_Base::BMD_SDIControl_I2C_Base;
  void setOverride(bool e) const { raw(&e, 1); }
  bool getOverride() const { return true; }
  bool setCameraTally(int cam, bool pgm, bool pvw) const {
    uint8_t b[3] = {(uint8_t)cam, (uint8_t)pgm, (uint8_t)pvw};
    raw(b, 3);
    return true;
  }
};

class BMD_SDICameraControl_I2C : public BMD_SDIControl_I2C_Base {
 public:
  using BMD_SDIControl_I2C_Base::BMD_SDIControl_I2C_Base;
  void setOverride(bool e) const { raw(&e, 1); }
  bool getOverride() const { return true; }

  void writeCommandVoid(byte c, byte g, byte p) const {
    uint8_t b[3] = {c, g, p};
    raw(b, 3);
  }
  void writeCommandBool(byte, byte, byte, byte, bool v) const { raw(&v, 1); }
  void writeCommandInt8(byte, byte, byte, byte, int8_t v) const { raw(&v, 1); }
  void writeCommandInt16(byte, byte, byte, byte, int16_t v) const { raw(&v, 2); }
  void writeCommandInt32(byte, byte, byte, byte, int32_t v) const { raw(&v, 4); }
  void writeCommandInt64(byte, byte, byte, byte, int64_t v) const { raw(&v, 8); }
  void writeCommandFixed16(byte, byte, byte, byte, float v) const {
    int16_t x = (int16_t)(v * 2048);
    raw(&x, 2);
  }
  void writeCommandUTF8(byte, byte, byte, byte, const char* s) const { raw(s, strlen(s)); }

  template <int N>
  void writeCommandBool(byte, byte, byte, byte, const bool (&v)[N]) const { raw(v, N); }
  template <int N>
  void writeCommandInt8(byte, byte, byte, byte, const int8_t (&v)[N]) const { raw(v, N); }
  template <int N>
  void writeCommandInt16(byte, byte, byte, byte, const int16_t (&v)[N]) const { raw(v, 2 * N); }
  template <int N>
  void writeCommandInt32(byte, byte, byte, byte, const int32_t (&v)[N]) const { raw(v, 4 * N); }
  template <int N>
  void writeCommandInt64(byte, byte, byte, byte, const int64_t (&v)[N]) const { raw(v, 8 * N); }
  template <int N>
  void writeCommandFixed16(byte, byte, byte, byte, const float (&v)[N]) const {
    int16_t x[N];
    for (int i = 0; i < N; i++) x[i] = (int16_t)(v[i] * 2048);
    raw(x, 2 * N);
  }
};

#endif  // BMDSDICONTROL_STUB_H
