/*
 * SdUtils.cpp
 * SD card utilities and preset management implementation
 * Version 3.7.1
 * 
 * Features:
 * - No String usage in critical functions
 * - Static buffer file reading
 * - In-place URL decode
 * - Direct client writing without intermediate buffer
 */

#include "SdUtils.h"
#include "CCUBroadcast.h"
#include "webserver.h"

// Static variable initialization
SdFat SdUtils::_sd;
File SdUtils::_presetFragmentedFile;
bool SdUtils::_presetFragmentedInProgress = false;
int SdUtils::_currentFragmentedPresetCamera = 0;
int SdUtils::_currentFragmentedPresetId = 0;
int SdUtils::_receivedFragmentsCount = 0;
uint8_t SdUtils::_sdBuffer[SD_BUFFER_SIZE];
char SdUtils::_lineBuffer[SD_LINE_BUFFER_SIZE];

static bool _applyingPreset = false;

// Keys never applied from a preset (TYPE_VOID actions are skipped by
// CCUControl). video_mode changes the SDI format; the rest are momentary
// commands/actions: velocities/speeds would leave the head or zoom moving,
// memory_preset would store/recall a PTZ memory, color bars go on air.
// Keep in step with NO_PRESET_PARAMS in sdcard/index.html.
static const char kSkipVideoMode[] PROGMEM = "video_mode";
static const char kSkipPanTilt[] PROGMEM = "pan_tilt_velocity";
static const char kSkipZoomSpeed[] PROGMEM = "set_continuous_zoom_speed";
static const char kSkipMemPreset[] PROGMEM = "memory_preset";
static const char kSkipColorBars[] PROGMEM = "color_bars_display_time_seconds";
static PGM_P const kPresetSkipKeys[] PROGMEM = {
  kSkipVideoMode, kSkipPanTilt, kSkipZoomSpeed, kSkipMemPreset, kSkipColorBars
};

static bool isPresetSkipKey(const char* key) {
  for (uint8_t i = 0; i < sizeof(kPresetSkipKeys) / sizeof(kPresetSkipKeys[0]); i++) {
    if (strcmp_P(key, (PGM_P)pgm_read_ptr(&kPresetSkipKeys[i])) == 0) return true;
  }
  return false;
}

bool SdUtils::isApplyingPreset() {
  return _applyingPreset;
}

bool SdUtils::begin() {
  pinMode(SD_PIN_CS, OUTPUT);
  digitalWrite(SD_PIN_CS, HIGH);
  
  if(!_sd.begin(SD_PIN_CS, SD_SCK_MHZ(50))) {
    Serial.println(F("Could not initialize SD"));
    return false;
  }
  
  Serial.println(F("SD initialized successfully"));
  return true;
}

SdFat& SdUtils::getSd() {
  return _sd;
}

bool SdUtils::fileExists(const char* filename) {
  return _sd.exists(filename);
}

bool SdUtils::removeFile(const char* filename) {
  return _sd.remove(filename);
}

bool SdUtils::renameFile(const char* oldName, const char* newName) {
  return _sd.rename(oldName, newName);
}

void SdUtils::optimizedWriteFile(File &file, const uint8_t *buffer, size_t size) {
  size_t remaining = size;
  size_t offset = 0;
  
  while (remaining > 0) {
    size_t blockSize = min(remaining, (size_t)SD_BUFFER_SIZE);
    memcpy(_sdBuffer, buffer + offset, blockSize);
    file.write(_sdBuffer, blockSize);
    offset += blockSize;
    remaining -= blockSize;
  }
}

void SdUtils::optimizedReadFile(File &file, uint8_t *buffer, size_t size) {
  size_t remaining = size;
  size_t offset = 0;
  
  while (remaining > 0) {
    size_t blockSize = min(remaining, (size_t)SD_BUFFER_SIZE);
    file.read(_sdBuffer, blockSize);
    memcpy(buffer + offset, _sdBuffer, blockSize);
    offset += blockSize;
    remaining -= blockSize;
  }
}

void SdUtils::getPresetFilename(char* buffer, int bufSize, int cameraId, int presetId, bool isTemp) {
  snprintf(buffer, bufSize, "preset_%d_%d.%s", cameraId, presetId, isTemp ? "tmp" : "dat");
}

void SdUtils::urlDecodeInPlace(char* str) {
  if (!str) return;
  
  int readPos = 0;
  int writePos = 0;
  
  while (str[readPos] != '\0') {
    if (str[readPos] == '%' && str[readPos + 1] != '\0' && str[readPos + 2] != '\0') {
      char hex[3] = { str[readPos + 1], str[readPos + 2], '\0' };
      str[writePos] = (char)strtol(hex, NULL, 16);
      readPos += 3;
    } else if (str[readPos] == '+') {
      str[writePos] = ' ';
      readPos++;
    } else {
      str[writePos] = str[readPos];
      readPos++;
    }
    writePos++;
  }
  str[writePos] = '\0';
}

String SdUtils::urlDecode(const String &input) {
  int len = input.length();
  if (len >= SD_LINE_BUFFER_SIZE) len = SD_LINE_BUFFER_SIZE - 1;
  
  strncpy(_lineBuffer, input.c_str(), len);
  _lineBuffer[len] = '\0';
  
  urlDecodeInPlace(_lineBuffer);
  
  return String(_lineBuffer);
}

bool SdUtils::savePreset(int cameraId, int presetId, const char* presetName, const char* presetData) {
  if (!tryLockSD(2000)) {
    Serial.println(F("Error: Could not access SD"));
    return false;
  }
  
  char filename[SD_PATH_BUFFER_SIZE];
  getPresetFilename(filename, SD_PATH_BUFFER_SIZE, cameraId, presetId, false);
  
  bool success = false;
  
  if (fileExists(filename) && !removeFile(filename)) {
    Serial.print(F("Error removing file: "));
    Serial.println(filename);
    unlockSD();
    return false;
  }
  
  File presetFile = _sd.open(filename, FILE_WRITE);
  if (!presetFile) {
    Serial.print(F("Error creating file: "));
    Serial.println(filename);
  } else {
    presetFile.print(F("cameraId:"));
    presetFile.println(cameraId);
    presetFile.print(F("presetId:"));
    presetFile.println(presetId);
    presetFile.print(F("name:"));
    presetFile.println(presetName ? presetName : "");
    if (presetData) presetFile.print(presetData);
    bool writeOk = !presetFile.getWriteError();
    // close() syncs to the card: its result counts too
    success = presetFile.close() && writeOk;
    
    if (success) {
      Serial.print(F("Preset saved: "));
    } else {
      Serial.print(F("Error writing preset: "));
    }
    Serial.println(filename);
  }
  
  unlockSD();
  return success;
}

// True only if the file was opened and fully read. Lines with unknown keys
// or malformed values are skipped (counted) but do not fail the preset.
bool SdUtils::applyPreset(int cameraId, int presetId) {
  // Safe Mode / shield not initialized: no I2C writes
  if (!CCUControl::isReady()) {
    return false;
  }
  
  if (cameraId < 1 || cameraId > MAX_CAMERAS || presetId < 0) {
    return false;
  }
  
  if (!tryLockSD(2000)) {
    Serial.println(F("Error: Could not access SD for preset"));
    return false;
  }
  
  char filename[SD_PATH_BUFFER_SIZE];
  getPresetFilename(filename, SD_PATH_BUFFER_SIZE, cameraId, presetId, false);
  
  if (!fileExists(filename)) {
    Serial.print(F("Preset not found: "));
    Serial.println(filename);
    unlockSD();
    return false;
  }
  
  File presetFile = _sd.open(filename);
  if (!presetFile) {
    Serial.println(F("Error opening preset"));
    unlockSD();
    return false;
  }
  
  _applyingPreset = true;
  
  Serial.print(F("Applying preset to camera "));
  Serial.println(cameraId);
  
  // Buffer for preset name
  char presetName[32] = "Preset";
  bool readError = false;
  int skipped = 0;
  
  // Read line by line to static buffer
  while (presetFile.available()) {
    int len = 0;
    bool overflow = false;
    
    while (presetFile.available()) {
      int c = presetFile.read();
      if (c < 0) {
        readError = true;
        break;
      }
      if (c == '\n' || c == '\r') break;
      if (len < SD_LINE_BUFFER_SIZE - 1) {
        _lineBuffer[len++] = (char)c;
      } else {
        overflow = true;   // keep reading to the end of the line
      }
    }
    if (readError) break;
    _lineBuffer[len] = '\0';
    
    // A truncated value could still parse as a wrong number: drop it
    if (overflow) {
      skipped++;
      continue;
    }
    
    if (len == 0) continue;
    
    char* colonPos = strchr(_lineBuffer, ':');
    if (!colonPos) continue;
    
    *colonPos = '\0';
    char* paramKey = _lineBuffer;
    char* paramValue = colonPos + 1;
    
    // Capture preset name
    if (strcmp_P(paramKey, PSTR("name")) == 0) {
      strncpy(presetName, paramValue, 31);
      presetName[31] = '\0';
      urlDecodeInPlace(presetName);
      continue;
    }
    
    // Skip other metadata
    if (strcmp_P(paramKey, PSTR("cameraId")) == 0 ||
        strcmp_P(paramKey, PSTR("presetId")) == 0) {
      continue;
    }
    
    if (isPresetSkipKey(paramKey)) {
#if DEBUG_HOT
      Serial.print(F("[Preset] skip "));
      Serial.println(paramKey);
#endif
      continue;
    }
    
    urlDecodeInPlace(paramValue);
    if (!CCUControl::applyParameterByKey(paramKey, paramValue, cameraId, true)) {
      skipped++;
#if DEBUG_HOT
      Serial.print(F("[Preset] rejected "));
      Serial.println(paramKey);
#endif
    }
  }
  
  presetFile.close();
  
  _applyingPreset = false;
  unlockSD();
  
  if (readError) {
    Serial.print(F("Error reading preset: "));
    Serial.println(filename);
    return false;
  }
  
  // Notify TCP clients (Companion)
  CCUBroadcast::sendPresetLoaded(cameraId, presetId, presetName);
  
  // Notify SSE client (web)
  WebServer::sendSSEPresetLoaded(cameraId, presetId, presetName);
  
  Serial.print(F("Preset applied"));
  if (skipped > 0) {
    Serial.print(F(", lines rejected: "));
    Serial.print(skipped);
  }
  Serial.println();
  return true;
}

void SdUtils::writePresetListToClient(EthernetClient &client) {
  File root = _sd.open("/");
  if (!root) {
    client.println(F("{\"presets\":[]}"));
    return;
  }
  
  client.print(F("{\"presets\":["));
  
  bool firstPreset = true;
  
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    
    char filename[SD_PATH_BUFFER_SIZE];
    entry.getName(filename, sizeof(filename));
    
    // Check pattern preset_X_Y.dat
    if (strncmp(filename, "preset_", 7) == 0 && strstr(filename, ".dat") != NULL) {
      int cameraId = 0, presetId = 0;
      sscanf(filename, "preset_%d_%d.dat", &cameraId, &presetId);
      
      char presetName[64] = "Preset";
      
      entry.seek(0);
      while (entry.available()) {
        int len = 0;
        while (entry.available() && len < SD_LINE_BUFFER_SIZE - 1) {
          char c = entry.read();
          if (c == '\n' || c == '\r') break;
          _lineBuffer[len++] = c;
        }
        _lineBuffer[len] = '\0';
        
        if (strncmp(_lineBuffer, "name:", 5) == 0) {
          strncpy(presetName, _lineBuffer + 5, sizeof(presetName) - 1);
          presetName[sizeof(presetName) - 1] = '\0';
          urlDecodeInPlace(presetName);
          break;
        }
      }
      
      if (!firstPreset) {
        client.print(F(","));
      }
      firstPreset = false;
      
      client.print(F("{\"cameraId\":"));
      client.print(cameraId);
      client.print(F(",\"presetId\":"));
      client.print(presetId);
      client.print(F(",\"name\":\""));
      WebServer::printJsonSafe(client, presetName);
      client.print(F("\"}"));
    }
    
    entry.close();
  }
  
  root.close();
  client.println(F("]}"));
}

bool SdUtils::initPresetFragmented(int cameraId, int presetId, const char* presetName) {
  if (_presetFragmentedInProgress && _presetFragmentedFile) {
    _presetFragmentedFile.close();
    _presetFragmentedInProgress = false;
  }
  
  _currentFragmentedPresetCamera = cameraId;
  _currentFragmentedPresetId = presetId;
  _receivedFragmentsCount = 0;
  
  char filename[SD_PATH_BUFFER_SIZE];
  getPresetFilename(filename, SD_PATH_BUFFER_SIZE, cameraId, presetId, true);
  
  if (fileExists(filename) && !removeFile(filename)) {
    Serial.println(F("ERROR: Could not remove temp file"));
    return false;
  }
  
  _presetFragmentedFile = _sd.open(filename, FILE_WRITE);
  if (!_presetFragmentedFile) {
    Serial.println(F("ERROR: Could not create temp file"));
    return false;
  }
  
  _presetFragmentedFile.print(F("cameraId:"));
  _presetFragmentedFile.println(cameraId);
  _presetFragmentedFile.print(F("presetId:"));
  _presetFragmentedFile.println(presetId);
  _presetFragmentedFile.print(F("name:"));
  _presetFragmentedFile.println(presetName ? presetName : "");
  _presetFragmentedFile.flush();
  
  if (_presetFragmentedFile.getWriteError()) {
    Serial.println(F("ERROR: Could not write temp file"));
    _presetFragmentedFile.close();
    removeFile(filename);
    return false;
  }
  
  _presetFragmentedInProgress = true;
  
  Serial.println(F("Fragmented preset started"));
  return true;
}

bool SdUtils::addPresetFragment(int cameraId, int presetId, int fragmentNumber, const char* fragmentData) {
  if (!_presetFragmentedInProgress || !_presetFragmentedFile) {
    Serial.println(F("ERROR: No preset being created"));
    return false;
  }
  
  if (cameraId != _currentFragmentedPresetCamera || presetId != _currentFragmentedPresetId) {
    Serial.println(F("ERROR: IDs don't match"));
    return false;
  }
  
  if (!fragmentData) {
    abortPresetFragmented();
    return false;
  }
  
  // A truncated fragment would store a cut value: fail the whole preset
  int dataLen = strlen(fragmentData);
  if (dataLen >= SD_LINE_BUFFER_SIZE) {
    Serial.println(F("ERROR: Fragment too long"));
    abortPresetFragmented();
    return false;
  }
  strncpy(_lineBuffer, fragmentData, dataLen);
  _lineBuffer[dataLen] = '\0';
  urlDecodeInPlace(_lineBuffer);
  
  int pairsProcessed = 0;
  char* ptr = _lineBuffer;
  
  while (*ptr != '\0') {
    char* semicolon = strchr(ptr, ';');
    if (!semicolon) break;
    
    *semicolon = '\0';
    _presetFragmentedFile.println(ptr);
    pairsProcessed++;
    
    ptr = semicolon + 1;
  }
  
  _receivedFragmentsCount++;
  _presetFragmentedFile.flush();
  
  if (_presetFragmentedFile.getWriteError()) {
    Serial.println(F("ERROR: Could not write fragment"));
    abortPresetFragmented();
    return false;
  }
  
  Serial.print(F("Frag #"));
  Serial.print(fragmentNumber);
  Serial.print(F(": "));
  Serial.print(pairsProcessed);
  Serial.println(F(" pairs"));

  return true;
}

// Drop a fragmented preset in progress (temp file removed, final file untouched)
void SdUtils::abortPresetFragmented() {
  if (_presetFragmentedFile) {
    _presetFragmentedFile.close();
  }
  char filename[SD_PATH_BUFFER_SIZE];
  getPresetFilename(filename, SD_PATH_BUFFER_SIZE, _currentFragmentedPresetCamera, _currentFragmentedPresetId, true);
  if (fileExists(filename)) {
    removeFile(filename);
  }
  _presetFragmentedInProgress = false;
  _currentFragmentedPresetCamera = 0;
  _currentFragmentedPresetId = 0;
  _receivedFragmentsCount = 0;
}

bool SdUtils::finalizePresetFragmented(int cameraId, int presetId, int totalFragments) {
  if (!_presetFragmentedInProgress) {
    Serial.println(F("ERROR: No preset being created"));
    return false;
  }
  
  if (cameraId != _currentFragmentedPresetCamera || presetId != _currentFragmentedPresetId) {
    Serial.println(F("ERROR: IDs don't match"));
    return false;
  }
  
  // Missing fragments or a failed final sync: keep the previous preset
  bool closeOk = true;
  if (_presetFragmentedFile) {
    closeOk = !_presetFragmentedFile.getWriteError();
    closeOk = _presetFragmentedFile.close() && closeOk;
  }
  if (!closeOk || (totalFragments > 0 && _receivedFragmentsCount != totalFragments)) {
    Serial.println(F("ERROR: Incomplete preset"));
    abortPresetFragmented();
    return false;
  }
  
  if (!tryLockSD(2000)) {
    Serial.println(F("ERROR: Could not access SD"));
    return false;   // temp file kept closed: finalize can be retried
  }
  
  bool success = false;
  
  char tempFilename[SD_PATH_BUFFER_SIZE];
  char finalFilename[SD_PATH_BUFFER_SIZE];
  getPresetFilename(tempFilename, SD_PATH_BUFFER_SIZE, cameraId, presetId, true);
  getPresetFilename(finalFilename, SD_PATH_BUFFER_SIZE, cameraId, presetId, false);
  
  if (_sd.exists(finalFilename)) {
    if (!_sd.remove(finalFilename)) {
      Serial.println(F("ERROR: Could not delete existing file"));
      unlockSD();
      abortPresetFragmented();
      return false;
    }
    delay(50);
  }
  
  if (_sd.rename(tempFilename, finalFilename)) {
    Serial.println(F("Fragmented preset finalized"));
    success = true;
  } else {
    Serial.println(F("ERROR: Could not rename file"));
  }
  
  _sd.begin(SD_PIN_CS, SD_SCK_MHZ(50));
  
  _presetFragmentedInProgress = false;
  _currentFragmentedPresetCamera = 0;
  _currentFragmentedPresetId = 0;
  _receivedFragmentsCount = 0;
  
  unlockSD();
  return success;
}

bool SdUtils::serveFile(EthernetClient &client, const char* filename) {
  File f = _sd.open(filename);
  if (!f) {
    client.println(F("HTTP/1.1 404 Not Found"));
    client.println(F("Content-Type: text/plain"));
    client.println(F("Connection: close"));
    client.println();
    client.println(F("404 Not Found"));
    return false;
  }
  
  client.println(F("HTTP/1.1 200 OK"));
  if (strstr(filename, ".html")) {
    client.println(F("Content-Type: text/html; charset=UTF-8"));
  } else if (strstr(filename, ".css")) {
    client.println(F("Content-Type: text/css"));
  } else if (strstr(filename, ".js")) {
    client.println(F("Content-Type: application/javascript"));
  } else if (strstr(filename, ".json")) {
    client.println(F("Content-Type: application/json"));
  } else if (strstr(filename, ".png")) {
    client.println(F("Content-Type: image/png"));
  } else if (strstr(filename, ".jpg") || strstr(filename, ".jpeg")) {
    client.println(F("Content-Type: image/jpeg"));
  } else if (strstr(filename, ".ico")) {
    client.println(F("Content-Type: image/x-icon"));
  } else {
    client.println(F("Content-Type: text/plain"));
  }
  client.println(F("Connection: close"));
  client.println();

  int n;
  while ((n = f.read(_sdBuffer, SD_BUFFER_SIZE)) > 0) {
    client.write(_sdBuffer, n);
  }
  
  f.close();
  
  // Periodically reinitialize
  static int fileCounter = 0;
  if (++fileCounter % 10 == 0) {
    _sd.begin(SD_PIN_CS, SD_SCK_MHZ(50));
  }
  
  return true;
}

bool SdUtils::tryLockSD(unsigned long timeout) {
  unsigned long startTime = millis();
  
  while (g_isBusyWithSD && (millis() - startTime < timeout)) {
    delay(1);
  }
  
  if (!g_isBusyWithSD) {
    g_isBusyWithSD = true;
    return true;
  }
  
  Serial.println(F("SD access timeout"));
  return false;
}

void SdUtils::unlockSD() {
  g_isBusyWithSD = false;
}
