/*
 * SafeMode.h
 * Safe mode detection and recovery system
 * 
 * Detects boot loops caused by:
 * - SDI Shield without power (12V)
 * - Invalid SDI input signal (4K, Level A, etc.)
 * - I2C communication failures
 * 
 * TallyCCU Pro V3.7.1
 */

#ifndef SAFEMODE_H
#define SAFEMODE_H

#include <Arduino.h>
#include <EEPROM.h>
#include <avr/wdt.h>

// ============================================================
// EEPROM ADDRESSES FOR SAFE MODE
// ============================================================
#define EEPROM_RESET_COUNT_ADDR     310   // Reset counter (1 byte)
#define EEPROM_RESET_FLAG_ADDR      311   // Reset flag magic (2 bytes)
#define EEPROM_RESET_REASON_ADDR    313   // Last reset reason (1 byte)
#define EEPROM_SAFEMODE_MANUAL_ADDR 314   // Manual safe mode flag (1 byte)

// ============================================================
// CONFIGURATION
// ============================================================
#define MAX_RESETS_BEFORE_SAFE_MODE 3     // Enter safe mode after N crashes
#define RESET_FLAG_MAGIC            0xDEAD // Magic number to detect watchdog reset
// Uptime (checked in loop()) after which a boot counts as successful. Any
// reset before it (hang, WDT, power cut, DTR reset from the configurator)
// counts as a boot failure; SafeMode::restart() does not. 20 s covers the
// first vMix tally, Companion reconnect (5 s) and browser SSE reconnect,
// while 3 external resets in a row inside the window stay unlikely.
#define BOOT_COMPLETE_DELAY_MS      20000UL

// ============================================================
// RESET REASONS
// ============================================================
enum ResetReason : uint8_t {
  RESET_UNKNOWN = 0,
  RESET_POWER_ON = 1,
  RESET_MANUAL = 2,
  RESET_WATCHDOG = 3,
  RESET_BROWNOUT = 4
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
  
  // Get diagnostic info
  static uint8_t getResetCount();
  static ResetReason getLastResetReason();
  static unsigned long getUptime();
  
  // Manual control
  static void exitSafeMode();       // Clear counters and restart
  static void enterSafeMode();      // Force safe mode on next boot
  static void forceNormalBoot();    // Force normal boot (for testing)
  static void restart();            // Intentional restart: not counted as a boot failure
  
  // Diagnostic strings
  static const char* getResetReasonString();
  static const char* getSafeModeReasonString();
  
private:
  static bool _safeMode;
  static uint8_t _resetCount;
  static ResetReason _lastResetReason;
  static bool _bootCompleted;
  
  // MCUSR helpers
  static ResetReason detectResetReason();
  static void clearResetFlags();
};

#endif // SAFEMODE_H
