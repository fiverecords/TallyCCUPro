/*
 * SafeMode.cpp
 * Safe mode detection and recovery system
 *
 * Reset cause (BOOT-06): the Mega bootloader clears MCUSR before the sketch
 * runs, so the cause comes from a boot record in .noinit RAM. Neither the C
 * runtime nor the bootloader touch it (the bootloader only uses its own
 * variables at the bottom of RAM and its stack at the top), so it survives
 * the watchdog, the reset pin and a jump to 0, and holds noise after a
 * power-up; a checksum tells which. The watchdog runs in interrupt + reset
 * mode: its interrupt marks the record (and where the code hung) and resets
 * at once. A hang with interrupts off never runs that interrupt and resets
 * at the second timeout without a mark: it is told from the reset pin by
 * the UART, which the Mega's stk500v2 bootloader sets up on every reset
 * except a watchdog one (it jumps straight to the sketch on WDRF). Only a
 * hang inside the boot window counts as a boot failure. Until RAM
 * retention has been seen once, a lost record cannot be told from a crash,
 * and the v3.8 EEPROM "boot in progress" flag is used as before.
 * Other bootloaders: with one that keeps MCUSR (Optiboot 8+) or none, the
 * cause is read from MCUSR. An old Optiboot (clears MCUSR, leaves through
 * a watchdog reset) makes the reset pin look like a hang: a reset inside
 * the boot window then counts as a failure, as in v3.8.
 */

#include "SafeMode.h"
#include "Configuration.h"
#include <Wire.h>
#include <stddef.h>
#include <avr/interrupt.h>

// Static variable initialization
bool SafeMode::_safeMode = false;
SafeModeCause SafeMode::_cause = SAFEMODE_OFF;
uint8_t SafeMode::_resetCount = 0;
ResetReason SafeMode::_lastResetReason = RESET_UNKNOWN;
bool SafeMode::_bootCompleted = false;
bool SafeMode::_legacyFlag = true;
uint8_t SafeMode::_strikes = 0;
bool SafeMode::_stable = false;
unsigned long SafeMode::_stableSince = 0;
unsigned long SafeMode::_lastProbe = 0;

// ============================================================
// BOOT RECORD (.noinit)
// ============================================================
#define BOOTREC_MAGIC 0x33424354UL   // "TCB3"
#define STACK_CANARY  0xC5

enum : uint8_t {
  BR_BOOTING = 1,   // boot window: a hang now is a boot failure
  BR_RUNNING = 2,   // boot completed
  BR_SAFE    = 3,   // Safe Mode running
  BR_RESTART = 4    // intentional restart in progress
};

struct BootRecord {
  uint32_t magic;
  uint32_t crashAddr;    // flash byte address the watchdog interrupted
  uint16_t seed;         // changes on every boot
  uint8_t  state;        // what this boot was doing
  uint8_t  wdtHit;       // set by the watchdog interrupt
  uint8_t  failCount;    // consecutive hangs inside the boot window
  uint8_t  crashCount;   // watchdog/crash resets since power-on
  uint8_t  check;
};

static BootRecord g_rec __attribute__((section(".noinit")));
// MCUSR and WDTCSR as found right after reset
static uint8_t g_resetMcusr __attribute__((section(".noinit")));
static uint8_t g_resetWdtcsr __attribute__((section(".noinit")));
// UCSR0B | UBRR0L as the bootloader left them: 0 after a watchdog reset
// with the stk500v2 bootloader, which then skips its UART setup
static uint8_t g_resetUart __attribute__((section(".noinit")));
// Set before the real reset that follows a jump to 0 (see captureResetCause)
static uint8_t g_softMark __attribute__((section(".noinit")));
#define SOFT_RESET_MARK 0xC3

static uint8_t recordCheck() {
  const uint8_t* p = (const uint8_t*)&g_rec;
  uint8_t c = 0x5A;
  for (uint8_t i = 0; i < offsetof(BootRecord, check); i++) {
    c = (uint8_t)((c << 1) | (c >> 7)) ^ p[i];
  }
  return c;
}

static bool recordValid() {
  return g_rec.magic == BOOTREC_MAGIC && g_rec.check == recordCheck();
}

static void recordSeal() {
  g_rec.magic = BOOTREC_MAGIC;
  g_rec.check = recordCheck();
}

static void recordSet(uint8_t state) {
  g_rec.state = state;
  recordSeal();
}

#if defined(__AVR__)
// Runs at reset before the C runtime init. MCUSR is cleared and the
// watchdog stopped at once: an old bootloader that leaves a watchdog reset
// armed (WDRF forces a ~16 ms timeout) would loop the board (BOOT-14).
// A watchdog still running with MCUSR clear means a jump to 0 (a crash)
// with every peripheral and pending interrupt as it was: reset the chip
// for real first. The mark stops a loop where the watchdog cannot be
// turned off (WDTON fuse).
static void captureResetCause() __attribute__((naked, used, section(".init3")));
static void captureResetCause() {
  uint8_t mcusr = MCUSR;
  uint8_t wdt = WDTCSR;
  if (mcusr == 0 && (wdt & (_BV(WDE) | _BV(WDIE))) && g_softMark != SOFT_RESET_MARK) {
    g_softMark = SOFT_RESET_MARK;
    wdt_enable(WDTO_15MS);
    for (;;) {}
  }
  g_resetMcusr = mcusr;
  g_resetWdtcsr = wdt;
  g_resetUart = UCSR0B | UBRR0L;   // before Serial.begin()
  MCUSR = 0;
  wdt_disable();
}

// Stack painting (MEM-02): every byte between the heap start and the top
// of RAM gets STACK_CANARY before the C runtime runs, so minFreeRam() can
// find the deepest point the stack ever reached. Basic asm: no stack, no r1.
#define SM_STR_(x) #x
#define SM_STR(x) SM_STR_(x)
static_assert(STACK_CANARY == 0xC5, "paintStack writes 0xC5");
static void paintStack() __attribute__((naked, used, section(".init1")));
static void paintStack() {
  __asm__ __volatile__(
    "cli\n\t"   // a jump to 0 may arrive with interrupts on
    "ldi r30, lo8(__heap_start)\n\t"
    "ldi r31, hi8(__heap_start)\n\t"
    "ldi r24, 0xC5\n\t"
    "ldi r25, hi8(" SM_STR(RAMEND) " + 1)\n\t"
    "1: st Z+, r24\n\t"
    "cpi r30, lo8(" SM_STR(RAMEND) " + 1)\n\t"
    "cpc r31, r25\n\t"
    "brlo 1b\n\t");
}
#endif

// Watchdog interrupt (interrupt + reset mode): a hang got this far. Note
// where and reset now instead of waiting for the second timeout.
extern "C" void safeModeWatchdogHit(uint32_t pcWord) __attribute__((used));
extern "C" void safeModeWatchdogHit(uint32_t pcWord) {
  g_rec.crashAddr = pcWord << 1;
  g_rec.wdtHit = 1;
  recordSeal();
  wdt_enable(WDTO_15MS);
#if defined(__AVR__)
  for (;;) {}
#endif
}

#if defined(__AVR__)
// Naked: read the interrupted PC (3 bytes, big-endian above SP) before any
// push, then jump to the handler; nothing returns from here.
ISR(WDT_vect, ISR_NAKED) {
  __asm__ __volatile__(
    "in r30, __SP_L__\n\t"
    "in r31, __SP_H__\n\t"
    "clr r1\n\t"
    "clr r25\n\t"
#if defined(__AVR_3_BYTE_PC__)
    "ldd r24, Z+1\n\t"
    "ldd r23, Z+2\n\t"
    "ldd r22, Z+3\n\t"
#else
    "clr r24\n\t"
    "ldd r23, Z+1\n\t"
    "ldd r22, Z+2\n\t"
#endif
    "jmp safeModeWatchdogHit\n\t");
}
#else
// Host emulator: called by a shim that models the watchdog interrupt (WDIE)
ISR(WDT_vect) {
  safeModeWatchdogHit(0);
}
#endif

// 8 s watchdog in interrupt + reset mode. WDIE needs no timed sequence;
// hardware clears it when the interrupt runs, so a hang inside an ISR or
// with interrupts off still resets at the second timeout (16 s).
static void armWatchdog() {
  wdt_enable(WDTO_8S);
  WDTCSR |= _BV(WDIE);
}

// ============================================================
// Uptime across the millis() wrap (BOOT-21)
// ============================================================
static uint32_t g_lastMs = 0;
static uint16_t g_wraps = 0;

static void trackUptime() {
  uint32_t now = millis();
  if (now < g_lastMs) g_wraps++;
  g_lastMs = now;
}

namespace {
// Wire.setWireTimeout() exists since AVR core 1.8.3, but WIRE_HAS_TIMEOUT
// is only defined by later cores (1.8.6 as installed here does not): detect
// the method itself so the timeout is not silently dropped.
template <class W>
auto setTimeoutIfAny(W& w, int) -> decltype(w.setWireTimeout(0UL, true), bool()) {
  w.setWireTimeout(SHIELD_WIRE_TIMEOUT_US, true);
  return true;
}
template <class W>
bool setTimeoutIfAny(W&, long) {
  return false;
}
}  // namespace

static bool legacyFlagArmed() {
  uint16_t flag;
  EEPROM.get(EEPROM_RESET_FLAG_ADDR, flag);
  return flag == RESET_FLAG_MAGIC;
}

static void setLegacyFlag(bool armed, uint8_t count) {
  EEPROM.put(EEPROM_RESET_FLAG_ADDR, (uint16_t)(armed ? RESET_FLAG_MAGIC : 0));
  EEPROM.update(EEPROM_RESET_COUNT_ADDR, count);
}

static void printHex32(uint32_t v) {
  Serial.print(F("0x"));
  for (int8_t s = 20; s >= 0; s -= 4) Serial.print((uint8_t)((v >> s) & 0x0F), HEX);
}

// ============================================================
// BOOT
// ============================================================

void SafeMode::begin() {
  // Disable watchdog FIRST (might still be active from previous crash)
  wdt_disable();
#if !defined(__AVR__)
  g_resetMcusr = MCUSR;
  g_resetWdtcsr = WDTCSR;
#if defined(UCSR0B) && defined(UBRR0L)
  g_resetUart = UCSR0B | UBRR0L;
#else
  g_resetUart = 0xFF;   // shim without UART registers: bootloader ran in full
#endif
  MCUSR = 0;
#endif

  classifyReset();

  // Enable watchdog EARLY - if shield init blocks, watchdog will catch it
  armWatchdog();

  Serial.print(F("[SafeMode] Safe mode: "));
  Serial.println(_safeMode ? F("ACTIVE") : F("OFF"));
}

void SafeMode::classifyReset() {
  uint8_t mcusr = g_resetMcusr;
  bool valid = recordValid();
  uint8_t prev = valid ? g_rec.state : 0;
  bool wdtHit = valid && g_rec.wdtHit;
  bool softReset = valid && g_softMark == SOFT_RESET_MARK;
  g_softMark = 0;

  ResetReason r;
  if (softReset)               r = RESET_CRASH;   // jump to 0, then a real reset
  else if (mcusr & _BV(WDRF))  r = (prev == BR_RESTART && !wdtHit) ? RESET_MANUAL : RESET_WATCHDOG;
  else if (mcusr & _BV(BORF))  r = RESET_BROWNOUT;
  else if (mcusr & _BV(EXTRF)) r = RESET_EXTERNAL;
  else if (mcusr & _BV(PORF))  r = RESET_POWER_ON;
  // MCUSR cleared by the bootloader (the usual case on a Mega). A watchdog
  // still running means nothing reset the chip (see captureResetCause)
  else if (g_resetWdtcsr & (_BV(WDE) | _BV(WDIE))) r = RESET_CRASH;
  else if (!valid)             r = RESET_POWER_ON;
  else if (wdtHit)             r = RESET_WATCHDOG;
  else if (prev == BR_RESTART) r = RESET_MANUAL;
  // No mark, but the bootloader skipped its UART setup: a watchdog reset
  // whose interrupt never ran (hang with interrupts off, e.g. inside the
  // TWI interrupt with SCL held low), at the second timeout
  else if (g_resetUart == 0)   r = RESET_WATCHDOG;
  // Power kept, bootloader ran in full: reset pin (button, serial DTR) or
  // a brown-out
  else                         r = RESET_EXTERNAL;
  bool crash = (r == RESET_WATCHDOG || r == RESET_CRASH);

  // A valid record after a reset proves that this board keeps RAM: from
  // then on a lost record means a real power-up (written once)
  bool kept = (EEPROM.read(EEPROM_BOOTREC_OK_ADDR) == 1);
  if (valid && !kept) {
    EEPROM.update(EEPROM_BOOTREC_OK_ADDR, 1);
    kept = true;
  }
  _legacyFlag = !kept;

  uint8_t fails;
  bool failure;
  if (valid) {
    fails = g_rec.failCount;
    failure = crash && prev == BR_BOOTING;
    if (crash && g_rec.crashCount < 255) g_rec.crashCount++;
    if (crash && !wdtHit) g_rec.crashAddr = 0;   // hang location unknown
    g_rec.seed = (uint16_t)(g_rec.seed * 25173U + 13849U);
  } else {
    // Power-up noise in the lost record seeds the boot seed
    uint16_t seed = 0;
    const uint8_t* p = (const uint8_t*)&g_rec;
    for (uint8_t i = 0; i < sizeof(g_rec); i++) seed = (uint16_t)(seed * 31U + p[i]);
    // Lost record: only the legacy EEPROM flag can show an interrupted boot,
    // and it cannot tell a power cut from a hang. Used until RAM retention
    // has been seen once.
    fails = EEPROM.read(EEPROM_RESET_COUNT_ADDR);
    failure = !kept && legacyFlagArmed();
    if (failure) r = RESET_UNKNOWN;
    g_rec.crashCount = crash ? 1 : 0;
    g_rec.crashAddr = 0;
    g_rec.seed = seed;
  }
  if (fails > 100) fails = 0;   // blank or corrupted
  fails = failure ? fails + 1 : 0;
  _lastResetReason = r;
  _resetCount = fails;

  Serial.print(F("[SafeMode] Last reset: "));
  Serial.print(getResetReasonString());
  if (crash) {
    if (g_rec.crashAddr) {
      Serial.print(F(" at "));
      printHex32(g_rec.crashAddr);
    }
    Serial.print(F(" ("));
    Serial.print(g_rec.crashCount);
    Serial.print(F(" since power-on, "));
    Serial.print(prev == BR_BOOTING ? F("during boot") : F("in operation"));
    Serial.print(')');
  }
  Serial.println();

  // Manual request (serial "safemode")
  if (EEPROM.read(EEPROM_SAFEMODE_MANUAL_ADDR) == SAFEMODE_MANUAL_MAGIC) {
    EEPROM.update(EEPROM_SAFEMODE_MANUAL_ADDR, 0);
    _safeMode = true;
    _cause = SAFEMODE_BY_REQUEST;
    _resetCount = MAX_RESETS_BEFORE_SAFE_MODE;
    fails = 0;
    Serial.println(F("[SafeMode] Manual safe mode requested"));
  } else if (failure) {
    Serial.print(F("[SafeMode] Boot failure detected! Count: "));
    Serial.println(fails);

    if (fails >= MAX_RESETS_BEFORE_SAFE_MODE) {
      _safeMode = true;
      _cause = SAFEMODE_BY_FAILURES;
      Serial.println(F("[SafeMode] Too many failures - ENTERING SAFE MODE"));

      // Anti-loop: these failed boots followed an auto-recovery
      if (EEPROM.read(EEPROM_SAFEMODE_AUTO_ADDR) == SAFEMODE_AUTO_MAGIC) {
        uint8_t strikes = EEPROM.read(EEPROM_SAFEMODE_STRIKES_ADDR);
        if (strikes > SAFEMODE_MAX_STRIKES) strikes = 0;   // blank EEPROM
        if (strikes < SAFEMODE_MAX_STRIKES) strikes++;
        EEPROM.update(EEPROM_SAFEMODE_STRIKES_ADDR, strikes);
        EEPROM.update(EEPROM_SAFEMODE_AUTO_ADDR, 0);
      }
      // Any later reset (power cycle, reboot) tries a normal boot with
      // the full count again (BOOT-08)
      fails = 0;
    }
  }

  // Boot window open (normal mode). Safe Mode never arms it: it does not
  // call bootComplete() (BOOT-08).
  g_rec.failCount = fails;
  g_rec.wdtHit = 0;
  recordSet(_safeMode ? BR_SAFE : BR_BOOTING);
  if (_legacyFlag) {
    setLegacyFlag(!_safeMode, fails);
  } else {
    setLegacyFlag(false, 0);   // writes nothing once cleared (BOOT-17)
  }

  _strikes = EEPROM.read(EEPROM_SAFEMODE_STRIKES_ADDR);
  if (_strikes > SAFEMODE_MAX_STRIKES) _strikes = 0;   // blank EEPROM
}

void SafeMode::bootComplete() {
  if (_bootCompleted) return;

  _bootCompleted = true;

  // We booted successfully: a later hang is an operation crash
  g_rec.failCount = 0;
  recordSet(BR_RUNNING);
  if (_legacyFlag) setLegacyFlag(false, 0);
  // A normal boot works again: re-arm the auto-recovery
  clearRecovery();

  Serial.println(F("[SafeMode] Boot completed successfully"));
}

bool SafeMode::isActive() {
  return _safeMode;
}

SafeModeCause SafeMode::getCause() {
  return _cause;
}

uint8_t SafeMode::getResetCount() {
  return _resetCount;
}

ResetReason SafeMode::getLastResetReason() {
  return _lastResetReason;
}

uint8_t SafeMode::getCrashCount() {
  return g_rec.crashCount;
}

uint32_t SafeMode::getCrashAddress() {
  return g_rec.crashAddr;
}

unsigned long SafeMode::getUptime() {
  trackUptime();
  uint32_t ms = g_lastMs;
  // 2^32 ms = 4294967 s + 296 ms per wrap
  return (unsigned long)g_wraps * 4294967UL + ms / 1000UL +
         (ms % 1000UL + (uint32_t)g_wraps * 296UL) / 1000UL;
}

uint16_t SafeMode::getBootSeed() {
  return g_rec.seed ^ (uint16_t)micros();
}

int SafeMode::freeRam() {
  extern int __heap_start, *__brkval;
  int v;
  uintptr_t low = (__brkval == 0) ? (uintptr_t)&__heap_start : (uintptr_t)__brkval;
  return (int)((uintptr_t)&v - low);
}

int SafeMode::minFreeRam() {
#if defined(__AVR__)
  extern int __heap_start, *__brkval;
  const uint8_t* p = (const uint8_t*)(__brkval == 0 ? &__heap_start : __brkval);
  const uint8_t* top = (const uint8_t*)SP;
  int n = 0;
  while (p < top && *p == STACK_CANARY) {
    p++;
    n++;
  }
  return n;
#else
  return -1;   // host emulator: not measurable
#endif
}

// ============================================================
// RESTARTS
// ============================================================

// Mark the restart as intentional and let the watchdog reset the board
void SafeMode::restartNow(uint8_t state) {
  recordSet(state);
  if (_legacyFlag) setLegacyFlag(false, 0);   // not a boot failure

  delay(100);

  // Trigger watchdog reset
  wdt_enable(WDTO_15MS);
  while(1);
}

void SafeMode::exitSafeMode() {
  Serial.println(F("[SafeMode] Clearing counters and restarting..."));

  // Clear all safe mode flags
  EEPROM.update(EEPROM_SAFEMODE_MANUAL_ADDR, 0);
  clearRecovery();
  restartNow(BR_RESTART);
}

void SafeMode::enterSafeMode() {
  Serial.println(F("[SafeMode] Restarting into Safe Mode now..."));
  EEPROM.update(EEPROM_SAFEMODE_MANUAL_ADDR, SAFEMODE_MANUAL_MAGIC);
  restartNow(BR_RESTART);
}

void SafeMode::restart() {
  // Intentional restart (web /reboot, serial "reset"): not a boot failure
  clearRecovery();   // manual action: re-arm the auto-recovery
  restartNow(BR_RESTART);
}

// ============================================================
// DIAGNOSTIC STRINGS
// ============================================================

// Copied to a small RAM buffer: callers keep a plain const char*
const char* SafeMode::getResetReasonString() {
  static char buf[10];
  PGM_P name;
  switch (_lastResetReason) {
    case RESET_POWER_ON: name = PSTR("Power On"); break;
    case RESET_MANUAL:   name = PSTR("Restart"); break;
    case RESET_WATCHDOG: name = PSTR("Watchdog"); break;
    case RESET_BROWNOUT: name = PSTR("Brown-out"); break;
    case RESET_EXTERNAL: name = PSTR("External"); break;
    case RESET_CRASH:    name = PSTR("Crash"); break;
    default:             name = PSTR("Unknown"); break;
  }
  strncpy_P(buf, name, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  return buf;
}

const __FlashStringHelper* SafeMode::getSafeModeReasonString() {
  switch (_cause) {
    case SAFEMODE_BY_REQUEST:  return F("Manual request");
    case SAFEMODE_BY_FAILURES: return F("Multiple watchdog resets (SDI Shield issue)");
    default:                   return F("Not in safe mode");
  }
}

void SafeMode::printFallbackPage(Print& out) {
  out.print(F("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
              "<meta name=\"viewport\" content=\"width=device-width\">"
              "<title>TallyCCU Pro - Safe Mode</title></head>"
              "<body style=\"font-family:sans-serif\"><h1>TallyCCU Pro: "));
  out.print(_safeMode ? F("Safe Mode") : F("Diagnostics"));
  out.print(F("</h1><p>The SD card page could not be read.</p><p>Reason: "));
  out.print(getSafeModeReasonString());
  out.print(F("<br>Last reset: "));
  out.print(getResetReasonString());
  out.print(F("<br>Failed boots: "));
  out.print(_resetCount);
  out.print(F("<br>Uptime: "));
  out.print(getUptime());
  out.print(F(" s</p>"));
  if (_safeMode) {
    out.print(F("<button onclick=\"fetch('/safemode-exit',{method:'POST',"
                "headers:{'X-TallyCCU':'1'}}).then(function(){"
                "document.body.textContent='Restarting...'})\">"
                "Restart in normal mode</button>"));
  }
  out.print(F("</body></html>"));
}

// ============================================================
// AUTO-RECOVERY (BOOT-01)
// ============================================================

bool SafeMode::armWireTimeout() {
#ifdef WIRE_HAS_TIMEOUT
  Wire.setWireTimeout(SHIELD_WIRE_TIMEOUT_US, true);
  return true;
#else
  return setTimeoutIfAny(Wire, 0);
#endif
}

bool SafeMode::autoRecoveryArmed() {
  return _safeMode && _cause == SAFEMODE_BY_FAILURES &&
         _strikes < SAFEMODE_MAX_STRIKES;
}

uint8_t SafeMode::getRecoveryStrikes() {
  return _strikes;
}

// Empty write to the shield address: ACK (0) means it is powered and on the
// bus. NACK returns at once; a stuck bus returns after the timeout.
bool SafeMode::probeShield() {
  static bool wireStarted = false;
  if (!wireStarted) {
    Wire.begin();   // keeps the timeout: twi_init() does not reset it
    wireStarted = true;
  }
  if (!armWireTimeout()) return false;   // never probe without a timeout
  Wire.beginTransmission(BMD_I2C_ADDRESS);
  return Wire.endTransmission() == 0;
}

void SafeMode::poll() {
  trackUptime();
  if (!autoRecoveryArmed()) return;

  unsigned long now = millis();
  if (now - _lastProbe < SAFEMODE_PROBE_MS) return;
  _lastProbe = now;

  if (!probeShield()) {
    if (_stable) Serial.println(F("[SafeMode] SDI shield lost"));
    _stable = false;
    return;
  }
  if (!_stable) {
    _stable = true;
    _stableSince = now;
    Serial.println(F("[SafeMode] SDI shield answers on I2C"));
    return;
  }

  unsigned long need = _strikes ? SAFEMODE_STABLE_RETRY_MS : SAFEMODE_STABLE_MS;
  if (now - _stableSince >= need) autoExit();
}

// Restart in normal mode, remembering that it was automatic: if this boot
// ends in Safe Mode again, begin() counts a strike
void SafeMode::autoExit() {
  Serial.println(F("[SafeMode] SDI shield stable, restarting in normal mode"));

  EEPROM.update(EEPROM_SAFEMODE_AUTO_ADDR, SAFEMODE_AUTO_MAGIC);
  restartNow(BR_RESTART);
}

void SafeMode::clearRecovery() {
  EEPROM.update(EEPROM_SAFEMODE_STRIKES_ADDR, 0);
  EEPROM.update(EEPROM_SAFEMODE_AUTO_ADDR, 0);
  _strikes = 0;
}
