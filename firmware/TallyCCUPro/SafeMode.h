/*
 * SafeMode.h
 * Safe mode detection and recovery system
 *
 * Detects boot loops caused by:
 * - SDI Shield without power (12V)
 * - Invalid SDI input signal (4K, Level A, etc.)
 * - I2C communication failures
 */

#ifndef SAFEMODE_H
#define SAFEMODE_H

#include <Arduino.h>
#include <EEPROM.h>
#include <avr/wdt.h>
#include "Configuration.h"   // EEPROM map (EEPROM_RESET_*, EEPROM_SAFEMODE_*)

// ============================================================
// CONFIGURATION
// ============================================================
#define MAX_RESETS_BEFORE_SAFE_MODE 3     // Enter safe mode after N crashes
#define RESET_FLAG_MAGIC            0xDEAD // legacy EEPROM "boot in progress" flag
// Uptime (checked in loop()) after which a boot counts as successful. A
// crash (watchdog, with or without its interrupt) before it is a boot
// failure; a power cut, the reset pin (button, serial DTR) or
// SafeMode::restart() are not (BOOT-06), once the board has shown that it
// keeps RAM across a reset (until then any reset inside the window counts,
// as in v3.8). Telling a watchdog reset without interrupt from the reset
// pin relies on the Mega's stk500v2 bootloader (see SafeMode.cpp). 20 s
// covers the first vMix tally, Companion reconnect (5 s) and browser SSE
// reconnect.
#define BOOT_COMPLETE_DELAY_MS      20000UL

// Auto-recovery (BOOT-01). In Safe Mode (entered after boot failures, not
// on request) the SDI shield is probed every SAFEMODE_PROBE_MS: an empty
// I2C write to its address with a bus timeout, so a dead or stuck bus
// cannot hang the loop. Once it has ACKed for SAFEMODE_STABLE_MS in a row
// the board restarts in normal mode. An ACK only shows that the shield is
// powered: a hang caused by the SDI input (no signal yet, 4K, Level A) is
// not detected beforehand. Anti-loop: if the boot after an auto-recovery
// falls back into Safe Mode, that is a strike; the next recovery needs
// SAFEMODE_STABLE_RETRY_MS, and after SAFEMODE_MAX_STRIKES it stays off
// until a manual exit (web or serial normal boot, reset) or a normal boot
// that completes, e.g. after a power cycle. Without Wire timeout support
// in the core the shield is never probed.
#define SAFEMODE_PROBE_MS           5000UL
#define SAFEMODE_STABLE_MS          30000UL
#define SAFEMODE_STABLE_RETRY_MS    300000UL
#define SAFEMODE_MAX_STRIKES        2
#define SAFEMODE_AUTO_MAGIC         0x5A
#define SAFEMODE_MANUAL_MAGIC       0xAA
// I2C timeout towards the shield (CCU-03, probe): 25 ms, bus reset
#define SHIELD_WIRE_TIMEOUT_US      25000UL

// ============================================================
// RESET REASONS (BOOT-06)
// ============================================================
// The Mega bootloader clears MCUSR, so the cause comes from a record in
// .noinit RAM (kept across every reset that keeps power) plus MCUSR when a
// bootloader leaves it set. See SafeMode.cpp.
enum ResetReason : uint8_t {
  RESET_UNKNOWN = 0,   // boot interrupted, cause unknown (RAM not kept yet)
  RESET_POWER_ON = 1,  // power-up (RAM content lost)
  RESET_MANUAL = 2,    // requested: web /reboot, serial, Safe Mode exit/entry, auto-recovery
  RESET_WATCHDOG = 3,  // hang: the watchdog fired (also with interrupts off)
  RESET_BROWNOUT = 4,  // supply dip (only when the bootloader keeps MCUSR)
  RESET_EXTERNAL = 5,  // power kept, no watchdog: reset pin (button, serial
                       // DTR) or a brown-out
  RESET_CRASH = 6      // jump to the reset vector without a hardware reset
};

// Why Safe Mode is active
enum SafeModeCause : uint8_t {
  SAFEMODE_OFF = 0,
  SAFEMODE_BY_REQUEST = 1,
  SAFEMODE_BY_FAILURES = 2
};

// ============================================================
// SAFE MODE CLASS
// ============================================================
class SafeMode {
public:
  // Initialize safe mode detection (call FIRST in setup!)
  static void begin();

  // Mark boot as complete (called from loop() after BOOT_COMPLETE_DELAY_MS;
  // repeated calls are ignored)
  static void bootComplete();

  // Check if in safe mode
  static bool isActive();
  static SafeModeCause getCause();

  // Called from every loop() pass: uptime tracking and, in Safe Mode, the
  // auto-recovery probe
  static void poll();
  static bool autoRecoveryArmed();
  static uint8_t getRecoveryStrikes();

  // Wire timeout towards the shield (SHIELD_WIRE_TIMEOUT_US, bus reset).
  // False if this core's Wire has no timeout.
  static bool armWireTimeout();

  // Get diagnostic info
  static uint8_t getResetCount();          // consecutive failed boots
  static ResetReason getLastResetReason();
  static uint8_t getCrashCount();          // watchdog/crash resets since power-on
  static uint32_t getCrashAddress();       // flash byte address of the last hang, 0 if unknown
  static unsigned long getUptime();        // seconds, survives the 49.7-day millis() wrap
  static uint16_t getBootSeed();           // varies on every boot (port randomization)

  // RAM (MEM-02): free now (between heap and stack) and the least that was
  // ever free since boot (stack painting; -1 when not measurable)
  static int freeRam();
  static int minFreeRam();

  // Manual control (all restart the board within ~115 ms)
  static void exitSafeMode();       // Clear counters and restart in normal mode
  static void enterSafeMode();      // Restart NOW into Safe Mode
  static void restart();            // Intentional restart: not counted as a boot failure

  // Diagnostic strings
  static const char* getResetReasonString();   // short, for JSON and logs
  static const __FlashStringHelper* getSafeModeReasonString();

  // Minimal diagnostics page for when safemode.html cannot be served
  // (BOOT-13): HTML body only, the caller sends the HTTP headers
  static void printFallbackPage(Print& out);

private:
  static bool _safeMode;
  static SafeModeCause _cause;
  static uint8_t _resetCount;
  static ResetReason _lastResetReason;
  static bool _bootCompleted;
  static bool _legacyFlag;
  static uint8_t _strikes;
  static bool _stable;
  static unsigned long _stableSince;
  static unsigned long _lastProbe;

  static void classifyReset();
  static bool probeShield();
  static void autoExit();
  static void clearRecovery();
  static void restartNow(uint8_t state);
};

#endif // SAFEMODE_H
