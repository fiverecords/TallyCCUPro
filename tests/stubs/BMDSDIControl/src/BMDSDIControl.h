// BMDSDIControl.h - COMPILE-ONLY STUB of the Blackmagic 3G-SDI Shield
// library (BMDSDIControl 1.0, 2016), for CI and local builds without it.
//
// The declarations copy the official library's, signature by signature
// (include/BMDSDICameraControl.h, BMDSDITallyControl.h, BMDSDIControlShield.h
// and BMDSDIControl.h): namespace BMD, I2CPhysical<T> built with the I2C
// address, begin() without arguments, array writes as template<unsigned N>
// taking NON-const T (&)[N], setCameraTally() returning void and no
// getOverride(). Keep it that way: a looser stub (const arrays, extra
// methods) builds code that does not compile with the real library, as
// happened with v3.8.
//
// Every call does a small I2C write so that code size and static RAM stay
// in the same range as with the real library (which is larger).
// A firmware built with this stub does NOT control cameras: never flash it.
#ifndef BMDSDICONTROL_STUB_H
#define BMDSDICONTROL_STUB_H

#include <Arduino.h>
#include <Wire.h>
#include <stdint.h>

#define BMDSDICONTROL_STUB 1

namespace BMD {

struct Version {
  uint16_t Major;
  uint16_t Minor;
};

class SDIControlShield {
 public:
  virtual void begin() const {}

  Version getLibraryVersion() const { return Version{1, 0}; }
  Version getProtocolVersion() const { return Version{0, 0}; }
  Version getFirmwareVersion() const { return Version{0, 0}; }

  void setOutputSignalOverride(bool enabled) const { regWrite8(0x0000, enabled); }
  bool getOutputSignalOverride() const { return regRead8(0x0000) != 0; }

  virtual uint32_t regRead32(uint16_t address) const = 0;
  virtual void regWrite32(uint16_t address, uint32_t value) const = 0;
  virtual uint16_t regRead16(uint16_t address) const = 0;
  virtual void regWrite16(uint16_t address, uint16_t value) const = 0;
  virtual uint8_t regRead8(uint16_t address) const = 0;
  virtual void regWrite8(uint16_t address, uint8_t value) const = 0;
  virtual void regRead(uint16_t address, uint8_t values[], int length) const = 0;
  virtual void regWrite(uint16_t address, const uint8_t values[], int length) const = 0;
};

class SDICameraControl : public SDIControlShield {
 public:
  void setOverride(bool enabled) const { regWrite8(0x4000, enabled); }

  bool available() const { return regRead8(0x4001) != 0; }
  template <unsigned N>
  int read(byte (&data)[N]) const { return read(data, N); }
  int read(byte data[], int dataLength) const {
    regRead(0x4100, data, dataLength);
    return dataLength;
  }
  void flushRead() const { regWrite8(0x4001, 0); }

  bool availableForWrite() const { return regRead8(0x4002) == 0; }
  template <unsigned N>
  void write(const byte (&data)[N]) const { write(data, N); }
  void write(const byte data[], int dataLength) const { regWrite(0x4200, data, dataLength); }
  void flushWrite() const { regWrite8(0x4002, 0); }

  void writeCommandVoid(byte camera, byte category, byte parameter) const {
    byte b[3] = {camera, category, parameter};
    write(b);
  }
  void writeCommandBool(byte camera, byte category, byte parameter, byte operation, bool value) const {
    byte b[5] = {camera, category, parameter, operation, value};
    write(b);
  }
  template <unsigned N>
  void writeCommandInt8(byte camera, byte category, byte parameter, byte operation, int8_t (&values)[N]) const {
    cmd(camera, category, parameter, operation, values, sizeof(values));
  }
  void writeCommandInt8(byte camera, byte category, byte parameter, byte operation, int8_t value) const {
    int8_t valueArray[] = {value};
    writeCommandInt8(camera, category, parameter, operation, valueArray);
  }
  template <unsigned N>
  void writeCommandInt16(byte camera, byte category, byte parameter, byte operation, int16_t (&values)[N]) const {
    cmd(camera, category, parameter, operation, values, sizeof(values));
  }
  void writeCommandInt16(byte camera, byte category, byte parameter, byte operation, int16_t value) const {
    int16_t valueArray[] = {value};
    writeCommandInt16(camera, category, parameter, operation, valueArray);
  }
  template <unsigned N>
  void writeCommandInt32(byte camera, byte category, byte parameter, byte operation, int32_t (&values)[N]) const {
    cmd(camera, category, parameter, operation, values, sizeof(values));
  }
  void writeCommandInt32(byte camera, byte category, byte parameter, byte operation, int32_t value) const {
    int32_t valueArray[] = {value};
    writeCommandInt32(camera, category, parameter, operation, valueArray);
  }
  template <unsigned N>
  void writeCommandInt64(byte camera, byte category, byte parameter, byte operation, int64_t (&values)[N]) const {
    cmd(camera, category, parameter, operation, values, sizeof(values));
  }
  void writeCommandInt64(byte camera, byte category, byte parameter, byte operation, int64_t value) const {
    int64_t valueArray[] = {value};
    writeCommandInt64(camera, category, parameter, operation, valueArray);
  }
  void writeCommandUTF8(byte camera, byte category, byte parameter, byte operation, const char* string) const {
    cmd(camera, category, parameter, operation, string, strlen(string));
  }
  void writeCommandUTF8(byte camera, byte category, byte parameter, byte operation, const String& string) const {
    writeCommandUTF8(camera, category, parameter, operation, string.c_str());
  }
  template <unsigned N>
  void writeCommandFixed16(byte camera, byte category, byte parameter, byte operation, float (&values)[N]) const {
    int16_t fixed[N];
    for (unsigned i = 0; i < N; i++) fixed[i] = toFixed16(values[i]);
    cmd(camera, category, parameter, operation, fixed, sizeof(fixed));
  }
  void writeCommandFixed16(byte camera, byte category, byte parameter, byte operation, float value) const {
    float valueArray[] = {value};
    writeCommandFixed16(camera, category, parameter, operation, valueArray);
  }

  int16_t toFixed16(float value) const { return (int16_t)(value * (1 << 11)); }
  float fromFixed16(int16_t value) const { return (float)value / (1 << 11); }

 private:
  void cmd(byte camera, byte category, byte parameter, byte operation, const void* p, int n) const {
    byte h[4] = {camera, category, parameter, operation};
    write(h);
    write((const byte*)p, n);
  }
};

class SDITallyControl : public SDIControlShield {
 public:
  void setOverride(bool enabled) const { regWrite8(0x3000, enabled); }

  bool available() const { return regRead8(0x3001) != 0; }
  int read(byte data[], int dataLength) const {
    regRead(0x3100, data, dataLength);
    return dataLength;
  }
  template <unsigned N>
  int read(byte (&data)[N]) const { return read(data, N); }
  void flushRead() const { regWrite8(0x3001, 0); }

  bool availableForWrite() const { return regRead8(0x3002) == 0; }
  void write(const byte data[], int dataLength) const { regWrite(0x3200, data, dataLength); }
  template <unsigned N>
  void write(const byte (&data)[N]) const { write(data, N); }
  void flushWrite() const { regWrite8(0x3002, 0); }
  void reset() const { regWrite8(0x3003, 1); }

  bool getCameraTally(int cameraNumber, bool& programTally, bool& previewTally) const {
    byte b = regRead8(0x3100 + cameraNumber);
    programTally = b & 1;
    previewTally = b & 2;
    return true;
  }
  void setCameraTally(int cameraNumber, bool programTally, bool previewTally) const {
    regWrite8(0x3200 + cameraNumber, (programTally ? 1 : 0) | (previewTally ? 2 : 0));
  }
};

template <typename T>
class I2CPhysical : public T {
 public:
  explicit I2CPhysical(int wireAddress) : m_wireAddress(wireAddress) {}

  virtual void begin() const {
    Wire.begin();
    T::begin();
  }

  virtual uint32_t regRead32(uint16_t address) const {
    uint8_t b[4];
    regRead(address, b, 4);
    return b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
  }
  virtual void regWrite32(uint16_t address, uint32_t value) const { regWrite(address, (const uint8_t*)&value, 4); }
  virtual uint16_t regRead16(uint16_t address) const {
    uint8_t b[2];
    regRead(address, b, 2);
    return b[0] | ((uint16_t)b[1] << 8);
  }
  virtual void regWrite16(uint16_t address, uint16_t value) const { regWrite(address, (const uint8_t*)&value, 2); }
  virtual uint8_t regRead8(uint16_t address) const {
    uint8_t b;
    regRead(address, &b, 1);
    return b;
  }
  virtual void regWrite8(uint16_t address, uint8_t value) const { regWrite(address, &value, 1); }
  virtual void regRead(uint16_t address, uint8_t values[], int length) const {
    Wire.beginTransmission(m_wireAddress);
    Wire.write((uint8_t)address);
    Wire.write((uint8_t)(address >> 8));
    Wire.endTransmission();
    Wire.requestFrom(m_wireAddress, length);
    for (int i = 0; i < length; i++) values[i] = Wire.read();
  }
  virtual void regWrite(uint16_t address, const uint8_t values[], int length) const {
    Wire.beginTransmission(m_wireAddress);
    Wire.write((uint8_t)address);
    Wire.write((uint8_t)(address >> 8));
    Wire.write(values, length);
    Wire.endTransmission();
  }

 private:
  int m_wireAddress;
};

}  // namespace BMD

typedef BMD::Version BMD_Version;
typedef BMD::I2CPhysical<BMD::SDITallyControl> BMD_SDITallyControl_I2C;
typedef BMD::I2CPhysical<BMD::SDICameraControl> BMD_SDICameraControl_I2C;

#endif  // BMDSDICONTROL_STUB_H
