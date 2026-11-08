/*
 * TallyManager.cpp
 * Tally management implementation
 *
 * O(n) mapping with n=10 is fast enough and saves 256 bytes of RAM
 * Safe Mode support: SDI Shield not initialized when in safe mode
 */

#include "TallyManager.h"
#include "SafeMode.h"
#include <BMDSDIControl.h>

// Static variable initialization
byte TallyManager::_inputs[MAXTALLIES] = {0};
byte TallyManager::_cams[MAXTALLIES] = {0};
bool TallyManager::_overrideTally = true;
bool TallyManager::_reapply = false;

// Shield tally state, one bit per camera (bit 0 = camera 1): wanted program
// and preview, and the cameras whose last write the shield accepted. A
// camera outside okMask is written again on the next pass (TAL-12).
static uint8_t wantPgm = 0;
static uint8_t wantPvw = 0;
static uint8_t okMask = 0;          // 0: everything is (re)sent
static uint8_t refreshCam = 0;
static unsigned long lastRefreshMs = 0;

// vMix state of the mapped cameras, whatever the shield shows (UI-18)
static uint8_t airPgm = 0;
static uint8_t airPvw = 0;

// Blackmagic Tally Shield
static BMD_SDITallyControl_I2C sdiTallyControl(BMD_I2C_ADDRESS);

static_assert(MAX_CAMERAS <= 8, "tally state is one byte per camera set");

namespace {
// setCameraTally() returns bool in the BMD library; a void one counts as
// success, so this builds against either. Non-const T&: builds whether or
// not the library's methods are const-qualified.
template <class T>
auto tallyWrite(T& t, int cam, bool pgm, bool pvw, int)
    -> decltype(bool(t.setCameraTally(cam, pgm, pvw))) {
  return t.setCameraTally(cam, pgm, pvw);
}
template <class T>
bool tallyWrite(T& t, int cam, bool pgm, bool pvw, long) {
  t.setCameraTally(cam, pgm, pvw);
  return true;
}

// Override read back from the shield; without getOverride() a shield reset
// cannot be seen, and the override is simply sent again
template <class T>
auto overrideKept(T& t, int) -> decltype(bool(t.getOverride())) {
  return t.getOverride();
}
template <class T>
bool overrideKept(T& t, long) {
  t.setOverride(true);
  return true;
}
}  // namespace

// Write one camera's wanted state; okMask follows the result
static bool writeCamera(uint8_t camera) {
  uint8_t bit = (uint8_t)(1 << (camera - 1));
  bool program = wantPgm & bit;
  bool preview = wantPvw & bit;
  bool ok = tallyWrite(sdiTallyControl, camera, program, preview, 0);
  if (ok) okMask |= bit;
  else okMask &= (uint8_t)~bit;

  #if DEBUG_HOT
  Serial.print(F("[T] "));
  Serial.print(camera);
  Serial.print(program ? F(":P") : (preview ? F(":V") : F(":")));
  Serial.println(ok ? F("") : F(" FAILED"));
  #endif
  return ok;
}

// Write the cameras in todo. A dead or stuck bus costs up to the I2C
// timeout per write: stop after two failures in a row, the rest stays
// outside okMask and is retried later.
static void writeCameras(uint8_t todo) {
  okMask &= (uint8_t)~todo;
  uint8_t fails = 0;
  for (uint8_t camera = 1; camera <= MAX_CAMERAS; camera++) {
    if (!(todo & (1 << (camera - 1)))) continue;
    if (writeCamera(camera)) {
      fails = 0;
    } else if (++fails >= 2) {
      break;
    }
  }
}

bool TallyManager::begin() {
  loadMap();

  bool dummyCCU;
  StorageManager::loadOverrides(_overrideTally, dummyCCU);

  // Only initialize SDI shield if NOT in safe mode
  if (!SafeMode::isActive()) {
    sdiTallyControl.begin();
    sdiTallyControl.setOverride(_overrideTally);
  } else {
    Serial.println(F("[Tally] SDI Shield SKIPPED (Safe Mode)"));
  }

  okMask = 0;

  return true;
}

void TallyManager::loadMap() {
  StorageManager::loadTallyMap(_inputs, _cams);
  printCurrentMap();
}

void TallyManager::saveMap() {
  StorageManager::saveTallyMap(_inputs, _cams);
  Serial.println(F("Tally map saved to EEPROM"));
}

// Empty (0/0), half-set or out of range (255/255 on a blank EEPROM) slots
// are never evaluated (TAL-07)
bool TallyManager::validSlot(byte index) {
  return index < MAXTALLIES &&
         _inputs[index] >= 1 && _inputs[index] <= MAX_VMIX_INPUTS &&
         _cams[index] >= 1 && _cams[index] <= MAX_CAMERAS;
}

void TallyManager::setMapping(byte inputIndex, byte inputNumber, byte cameraId) {
  if (inputIndex < MAXTALLIES) {
    // 0 (or out of range) leaves that half of the slot unchanged
    if (inputNumber >= 1 && inputNumber <= MAX_VMIX_INPUTS) _inputs[inputIndex] = inputNumber;
    if (cameraId >= 1 && cameraId <= MAX_CAMERAS) _cams[inputIndex] = cameraId;
    // A half-set slot is stored empty: no hidden half that a later request
    // turns on, and getMapping() shows exactly what is stored
    if (!validSlot(inputIndex)) {
      _inputs[inputIndex] = 0;
      _cams[inputIndex] = 0;
    }
    saveMap();
    _reapply = true;
  }
}

void TallyManager::getMapping(byte index, byte &inputNumber, byte &cameraId) {
  if (validSlot(index)) {
    inputNumber = _inputs[index];
    cameraId = _cams[index];
  } else {
    inputNumber = 0;
    cameraId = 0;
  }
}

void TallyManager::setOverride(bool enabled) {
  _overrideTally = enabled;

  // Only touch SDI shield if NOT in safe mode
  if (!SafeMode::isActive()) {
    sdiTallyControl.setOverride(_overrideTally);
  }

  bool currentTally, currentCCU;
  StorageManager::loadOverrides(currentTally, currentCCU);
  StorageManager::saveOverrides(_overrideTally, currentCCU);

  Serial.print(F("Tally override: "));
  Serial.println(_overrideTally ? F("ON") : F("OFF"));

  // Full resend of the current vMix state on the next loop pass
  if (enabled) {
    okMask = 0;
    _reapply = true;
  }
}

bool TallyManager::getOverride() {
  return _overrideTally;
}

void TallyManager::printCurrentMap() {
  Serial.println(F("Current tally map:"));
  for (int i = 0; i < MAXTALLIES; i++) {
    if (validSlot(i)) {
      Serial.print(F("  Idx "));
      Serial.print(i);
      Serial.print(F(": In "));
      Serial.print(_inputs[i]);
      Serial.print(F(" => Cam "));
      Serial.println(_cams[i]);
    }
  }
}

void TallyManager::setTallyStates(bool states[MAX_CAMERAS + 1][2]) {
  uint8_t pgm = 0, pvw = 0;
  for (uint8_t camera = 1; camera <= MAX_CAMERAS; camera++) {
    uint8_t bit = (uint8_t)(1 << (camera - 1));
    if (states[camera][0]) pgm |= bit;
    if (states[camera][1]) pvw |= bit;
  }
  airPgm = pgm;
  airPvw = pvw;

  // No SDI output with the override off or in Safe Mode
  if (!_overrideTally || SafeMode::isActive()) {
    return;
  }

  // Changed cameras, plus any whose last write failed
  uint8_t todo = (uint8_t)((pgm ^ wantPgm) | (pvw ^ wantPvw) | ~okMask);
  wantPgm = pgm;
  wantPvw = pvw;
  writeCameras(todo);
}

void TallyManager::clearAll() {
  bool states[MAX_CAMERAS + 1][2] = {{false}};
  setTallyStates(states);
}

void TallyManager::getAir(uint8_t &pgm, uint8_t &pvw) {
  pgm = airPgm;
  pvw = airPvw;
}

// One camera slot per call, round robin (a round takes MAX_CAMERAS *
// TALLY_REFRESH_MS). Mapped cameras and failed writes are sent again, so a
// write the shield lost heals within a round. At the start of a round the
// override is read back: a shield that reset (override and tally cleared)
// gets both again at once.
void TallyManager::refresh() {
#if TALLY_REFRESH_MS > 0
  if (!_overrideTally || SafeMode::isActive()) return;

  unsigned long now = millis();
  if (now - lastRefreshMs < TALLY_REFRESH_MS) return;
  lastRefreshMs = now;

  if (++refreshCam > MAX_CAMERAS) {
    refreshCam = 1;
    static bool lost = false;
    if (!overrideKept(sdiTallyControl, 0)) {
      if (!lost) Serial.println(F("[Tally] Shield lost its settings (reset?), sending them again"));
      lost = true;
      sdiTallyControl.setOverride(true);
      writeCameras(0xFF);
      return;
    }
    lost = false;
  }

  uint8_t bit = (uint8_t)(1 << (refreshCam - 1));
  uint8_t mapped = 0;
  for (byte i = 0; i < MAXTALLIES; i++) {
    if (validSlot(i)) mapped |= (uint8_t)(1 << (_cams[i] - 1));
  }
  if ((mapped & bit) || !(okMask & bit)) writeCamera(refreshCam);
#endif
}

void TallyManager::clearMapping(byte index) {
  if (index < MAXTALLIES) {
    _inputs[index] = 0;
    _cams[index] = 0;
    saveMap();
    _reapply = true;
  }
}

bool TallyManager::takeReapply() {
  bool r = _reapply;
  _reapply = false;
  return r;
}
