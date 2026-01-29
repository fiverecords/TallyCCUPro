/*
 * SafeMode.cpp
 * Safe mode detection and recovery system
 * 
 * TallyCCU Pro V3.7
 * 
 * NOTE: Arduino bootloader clears MCUSR before sketch runs,
 * so we cannot rely on it to detect watchdog resets.
 * Instead, we use EEPROM flags to track boot failures.
 */

#include "SafeMode.h"

// Static variable initialization
bool SafeMode::_safeMode = false;
uint8_t SafeMode::_resetCount = 0;
ResetReason SafeMode::_lastResetReason = RESET_UNKNOWN;
bool SafeMode::_bootCompleted = false;

void SafeMode::begin() {
  // Disable watchdog FIRST (might still be active from previous crash)
  wdt_disable();
  
  // Clear MCUSR (though bootloader likely already did)
  MCUSR = 0;
  
  // Check for manual safe mode request
  uint8_t manualFlag = EEPROM.read(EEPROM_SAFEMODE_MANUAL_ADDR);
  if (manualFlag == 0xAA) {
    EEPROM.write(EEPROM_SAFEMODE_MANUAL_ADDR, 0);
    _safeMode = true;
    _resetCount = MAX_RESETS_BEFORE_SAFE_MODE;
    _lastResetReason = RESET_MANUAL;
    Serial.println(F("[SafeMode] Manual safe mode requested"));
    wdt_enable(WDTO_8S);
    return;
  }
  
  // Read stored reset tracking data
  uint16_t resetFlag;
  EEPROM.get(EEPROM_RESET_FLAG_ADDR, resetFlag);
  _resetCount = EEPROM.read(EEPROM_RESET_COUNT_ADDR);
  
  // Validate reset count (corrupted EEPROM protection)
  if (_resetCount > 100) {
    _resetCount = 0;
  }
  
  // KEY LOGIC: If magic flag is present, previous boot didn't complete = CRASH
  // We don't rely on MCUSR because Arduino bootloader clears it
  if (resetFlag == RESET_FLAG_MAGIC) {
    // Previous boot didn't complete - this is a crash/watchdog recovery
    _lastResetReason = RESET_WATCHDOG;
    _resetCount++;
    EEPROM.write(EEPROM_RESET_COUNT_ADDR, _resetCount);
    
    Serial.print(F("[SafeMode] Boot failure detected! Count: "));
    Serial.println(_resetCount);
    
    // Check if we should enter safe mode
    if (_resetCount >= MAX_RESETS_BEFORE_SAFE_MODE) {
      _safeMode = true;
      Serial.println(F("[SafeMode] Too many failures - ENTERING SAFE MODE"));
      
      // Clear the flag so manual power cycle can try normal boot
      EEPROM.put(EEPROM_RESET_FLAG_ADDR, (uint16_t)0);
      EEPROM.write(EEPROM_RESET_COUNT_ADDR, 0);
    }
  } else {
    // Magic flag NOT present = clean boot (power cycle or successful previous boot)
    _resetCount = 0;
    _lastResetReason = RESET_POWER_ON;
    EEPROM.write(EEPROM_RESET_COUNT_ADDR, 0);
    
    Serial.println(F("[SafeMode] Clean boot"));
  }
  
  // Set flag indicating we're starting boot process
  // This will be cleared ONLY by bootComplete() if we succeed
  // If we crash before that, next boot will see this flag = failure
  EEPROM.put(EEPROM_RESET_FLAG_ADDR, (uint16_t)RESET_FLAG_MAGIC);
  
  // Enable watchdog EARLY - if shield init blocks, watchdog will catch it
  wdt_enable(WDTO_8S);
  
  Serial.print(F("[SafeMode] Safe mode: "));
  Serial.println(_safeMode ? F("ACTIVE") : F("OFF"));
}

void SafeMode::bootComplete() {
  if (_bootCompleted) return;
  
  _bootCompleted = true;
  
  // Clear the crash detection flag - we booted successfully!
  EEPROM.put(EEPROM_RESET_FLAG_ADDR, (uint16_t)0);
  EEPROM.write(EEPROM_RESET_COUNT_ADDR, 0);
  
  Serial.println(F("[SafeMode] Boot completed successfully"));
}

bool SafeMode::isActive() {
  return _safeMode;
}

uint8_t SafeMode::getResetCount() {
  return _resetCount;
}

ResetReason SafeMode::getLastResetReason() {
  return _lastResetReason;
}

unsigned long SafeMode::getUptime() {
  return millis() / 1000;
}

void SafeMode::exitSafeMode() {
  Serial.println(F("[SafeMode] Clearing counters and restarting..."));
  
  // Clear all safe mode flags
  EEPROM.put(EEPROM_RESET_FLAG_ADDR, (uint16_t)0);
  EEPROM.write(EEPROM_RESET_COUNT_ADDR, 0);
  EEPROM.write(EEPROM_SAFEMODE_MANUAL_ADDR, 0);
  
  delay(100);
  
  // Trigger watchdog reset
  wdt_enable(WDTO_15MS);
  while(1);
}

void SafeMode::enterSafeMode() {
  Serial.println(F("[SafeMode] Manual safe mode on next boot..."));
  
  // Set manual safe mode flag
  EEPROM.write(EEPROM_SAFEMODE_MANUAL_ADDR, 0xAA);
  
  delay(100);
  
  // Trigger watchdog reset
  wdt_enable(WDTO_15MS);
  while(1);
}

void SafeMode::forceNormalBoot() {
  Serial.println(F("[SafeMode] Forcing normal boot..."));
  
  // Clear everything but DON'T set the boot flag
  EEPROM.put(EEPROM_RESET_FLAG_ADDR, (uint16_t)0);
  EEPROM.write(EEPROM_RESET_COUNT_ADDR, 0);
  EEPROM.write(EEPROM_SAFEMODE_MANUAL_ADDR, 0);
  
  delay(100);
  
  // Trigger watchdog reset
  wdt_enable(WDTO_15MS);
  while(1);
}

ResetReason SafeMode::detectResetReason() {
  uint8_t mcusr = MCUSR;
  
  // Check flags in order of priority
  if (mcusr & (1 << WDRF)) {
    return RESET_WATCHDOG;
  }
  if (mcusr & (1 << BORF)) {
    return RESET_BROWNOUT;
  }
  if (mcusr & (1 << EXTRF)) {
    return RESET_MANUAL;
  }
  if (mcusr & (1 << PORF)) {
    return RESET_POWER_ON;
  }
  
  return RESET_UNKNOWN;
}

void SafeMode::clearResetFlags() {
  MCUSR = 0;
}

const char* SafeMode::getResetReasonString() {
  switch (_lastResetReason) {
    case RESET_POWER_ON:  return "Power On";
    case RESET_MANUAL:    return "Manual Reset";
    case RESET_WATCHDOG:  return "Watchdog";
    case RESET_BROWNOUT:  return "Brown-out";
    default:              return "Unknown";
  }
}

const char* SafeMode::getSafeModeReasonString() {
  if (!_safeMode) {
    return "Not in safe mode";
  }
  
  uint8_t manualFlag = EEPROM.read(EEPROM_SAFEMODE_MANUAL_ADDR);
  if (manualFlag == 0xAA) {
    return "Manual request";
  }
  
  return "Multiple watchdog resets (SDI Shield issue)";
}
