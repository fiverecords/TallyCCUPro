/* BMDSDIControl.h - HOST shim of the Blackmagic 3G-SDI Shield library.
 * Array writes have the official signatures, template<unsigned N> on a
 * NON-const T (&)[N], so const arrays fail here as with the real library.
 * Beyond the official API (see tests/stubs/BMDSDIControl), setCameraTally()
 * returns bool and getOverride() exists, to drive the firmware paths for
 * libraries that report tally failures and shield resets. Every call is logged
 * as a JSON line to EMU_BMD_LOG ("what reaches the camera").
 * Arrays: all N elements are logged ("n"); elements past the parameter's
 * subCount may be uninitialised stack memory in the firmware. Fixed16 values
 * are logged as float and as the 5.11 int16 the shield would encode
 * (round-toward-zero of v*2048, wrapped to 16 bits like the stub). */
#ifndef EMU_BMDSDICONTROL_H
#define EMU_BMDSDICONTROL_H
#include <Arduino.h>
#include <Wire.h>

void emu_shield_hang();
bool emu_op_hang();
bool emu_tally_fail();
bool emu_shield_reset_pending();
void emu_shield_reset_clear();
void emu_bmd_log(const char *fn, int cam, int g, int p, int op, const char *vals, int n);
void emu_bmd_fmt_f(char *out, size_t sz, const float *v, int n, bool fixed);
void emu_bmd_fmt_i(char *out, size_t sz, const long long *v, int n);

class BMD_SDIControl_I2C_Base {
 public:
  explicit BMD_SDIControl_I2C_Base(int addr) : _addr(addr) {}
  void begin() { Wire.begin(); emu_bmd_log("begin", -1, -1, -1, -1, "[]", 0); while (!emu_shield_present()) emu_shield_hang(); }
  unsigned long getFirmwareVersion() const { return 0x0100; }
  unsigned long getProtocolVersion() const { return 0x0100; }
 protected:
  int _addr;
};

class BMD_SDITallyControl_I2C : public BMD_SDIControl_I2C_Base {
 public:
  using BMD_SDIControl_I2C_Base::BMD_SDIControl_I2C_Base;
  void setOverride(bool e) const { emu_shield_reset_clear(); emu_bmd_log("tallySetOverride", -1, -1, -1, -1, e ? "[1]" : "[0]", 1); }
  bool getOverride() const { return !emu_shield_reset_pending(); }
  bool setCameraTally(int cam, bool pgm, bool pvw) const {
    while (emu_op_hang()) emu_shield_hang();
    bool ok = !emu_tally_fail();
    char b[16]; snprintf(b, sizeof b, "[%d,%d]", pgm ? 1 : 0, pvw ? 1 : 0);
    emu_bmd_log(ok ? "setCameraTally" : "setCameraTallyFailed", cam, -1, -1, -1, b, 2); return ok;
  }
};

class BMD_SDICameraControl_I2C : public BMD_SDIControl_I2C_Base {
 public:
  using BMD_SDIControl_I2C_Base::BMD_SDIControl_I2C_Base;
  void setOverride(bool e) const { emu_bmd_log("cameraSetOverride", -1, -1, -1, -1, e ? "[1]" : "[0]", 1); }
  bool getOverride() const { return true; }
  void writeCommandVoid(byte c, byte g, byte p) const { emu_bmd_log("writeCommandVoid", c, g, p, 0, "[]", 0); }
  void writeCommandBool(byte c, byte g, byte p, byte op, bool v) const { emu_bmd_log("writeCommandBool", c, g, p, op, v ? "[1]" : "[0]", 1); }
  void writeCommandInt8(byte c, byte g, byte p, byte op, int8_t v) const { li("writeCommandInt8", c, g, p, op, v); }
  void writeCommandInt16(byte c, byte g, byte p, byte op, int16_t v) const { li("writeCommandInt16", c, g, p, op, v); }
  void writeCommandInt32(byte c, byte g, byte p, byte op, int32_t v) const { li("writeCommandInt32", c, g, p, op, v); }
  void writeCommandInt64(byte c, byte g, byte p, byte op, int64_t v) const { li("writeCommandInt64", c, g, p, op, v); }
  void writeCommandFixed16(byte c, byte g, byte p, byte op, float v) const {
    char b[96]; emu_bmd_fmt_f(b, sizeof b, &v, 1, true); emu_bmd_log("writeCommandFixed16", c, g, p, op, b, 1);
  }
  void writeCommandUTF8(byte c, byte g, byte p, byte op, const char *s) const {
    char b[200]; snprintf(b, sizeof b, "[\"%s\"]", s ? s : ""); emu_bmd_log("writeCommandUTF8", c, g, p, op, b, 1);
  }
  template <unsigned N> void writeCommandInt8(byte c, byte g, byte p, byte op, int8_t (&v)[N]) const {
    long long t[N]; for (unsigned i = 0; i < N; i++) t[i] = v[i]; la("writeCommandInt8", c, g, p, op, t, N);
  }
  template <unsigned N> void writeCommandInt16(byte c, byte g, byte p, byte op, int16_t (&v)[N]) const {
    long long t[N]; for (unsigned i = 0; i < N; i++) t[i] = v[i]; la("writeCommandInt16", c, g, p, op, t, N);
  }
  template <unsigned N> void writeCommandInt32(byte c, byte g, byte p, byte op, int32_t (&v)[N]) const {
    long long t[N]; for (unsigned i = 0; i < N; i++) t[i] = v[i]; la("writeCommandInt32", c, g, p, op, t, N);
  }
  template <unsigned N> void writeCommandInt64(byte c, byte g, byte p, byte op, int64_t (&v)[N]) const {
    long long t[N]; for (unsigned i = 0; i < N; i++) t[i] = v[i]; la("writeCommandInt64", c, g, p, op, t, N);
  }
  template <unsigned N> void writeCommandFixed16(byte c, byte g, byte p, byte op, float (&v)[N]) const {
    char b[600]; emu_bmd_fmt_f(b, sizeof b, v, N, true); emu_bmd_log("writeCommandFixed16", c, g, p, op, b, N);
  }
 private:
  void li(const char *fn, int c, int g, int p, int op, long long v) const {
    char b[48]; emu_bmd_fmt_i(b, sizeof b, &v, 1); emu_bmd_log(fn, c, g, p, op, b, 1);
  }
  void la(const char *fn, int c, int g, int p, int op, const long long *t, int n) const {
    char b[600]; emu_bmd_fmt_i(b, sizeof b, t, n); emu_bmd_log(fn, c, g, p, op, b, n);
  }
};
#endif
