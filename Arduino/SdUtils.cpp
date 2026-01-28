/*
 * SdUtils.cpp
 * SD card utilities and preset management implementation
 * Version 3.6
 * 
 * Features:
 * - No String usage in critical functions
 * - Static buffer file reading
 * - In-place URL decode
 * - Direct client writing without intermediate buffer
 */

#include "SdUtils.h"
#include "CCUBroadcast.h"
#include "WebServer.h"

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
  
  if (fileExists(filename)) {
    removeFile(filename);
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
    presetFile.println(presetName);
    presetFile.print(presetData);
    presetFile.flush();
    presetFile.close();
    
    Serial.print(F("Preset saved: "));
    Serial.println(filename);
    success = true;
  }
  
  unlockSD();
  return success;
}

bool SdUtils::applyPreset(int cameraId, int presetId) {
  char filename[SD_PATH_BUFFER_SIZE];
  getPresetFilename(filename, SD_PATH_BUFFER_SIZE, cameraId, presetId, false);
  
  if (!fileExists(filename)) {
    Serial.print(F("Preset not found: "));
    Serial.println(filename);
    return false;
  }
  
  File presetFile = _sd.open(filename);
  if (!presetFile) {
    Serial.println(F("Error opening preset"));
    return false;
  }
  
  _applyingPreset = true;
  
  int originalCameraId = CCUControl::getActiveCamera();
  CCUControl::setActiveCamera(cameraId);
  
  Serial.print(F("Applying preset to camera "));
  Serial.println(cameraId);
  
  // Buffer for preset name
  char presetName[32] = "Preset";
  
  // Read line by line to static buffer
  while (presetFile.available()) {
    int len = 0;
    
    while (presetFile.available() && len < SD_LINE_BUFFER_SIZE - 1) {
      char c = presetFile.read();
      if (c == '\n' || c == '\r') break;
      _lineBuffer[len++] = c;
    }
    _lineBuffer[len] = '\0';
    
    if (len == 0) continue;
    
    char* colonPos = strchr(_lineBuffer, ':');
    if (!colonPos) continue;
    
    *colonPos = '\0';
    char* paramKey = _lineBuffer;
    char* paramValue = colonPos + 1;
    
    // Capture preset name
    if (strcmp(paramKey, "name") == 0) {
      strncpy(presetName, paramValue, 31);
      presetName[31] = '\0';
      urlDecodeInPlace(presetName);
      continue;
    }
    
    // Skip other metadata
    if (strcmp(paramKey, "cameraId") == 0 ||
        strcmp(paramKey, "presetId") == 0) {
      continue;
    }
    
    urlDecodeInPlace(paramValue);
    CCUControl::applyParameterByKey(paramKey, paramValue);
  }
  
  presetFile.close();
  CCUControl::setActiveCamera(originalCameraId);
  
  _applyingPreset = false;
  
  // Notify TCP clients (Companion)
  CCUBroadcast::sendPresetLoaded(cameraId, presetId, presetName);
  
  // Notify SSE client (web)
  WebServer::sendSSEPresetLoaded(cameraId, presetId, presetName);
  
  Serial.println(F("Preset applied"));
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
      client.print(presetName);
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
  
  if (fileExists(filename)) {
    removeFile(filename);
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
  _presetFragmentedFile.println(presetName);
  _presetFragmentedFile.flush();
  
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
  
  if (!_presetFragmentedFile) {
    char filename[SD_PATH_BUFFER_SIZE];
    getPresetFilename(filename, SD_PATH_BUFFER_SIZE, cameraId, presetId, true);
    _presetFragmentedFile = _sd.open(filename, FILE_WRITE);
    if (!_presetFragmentedFile) {
      _presetFragmentedInProgress = false;
      return false;
    }
    _presetFragmentedFile.seek(_presetFragmentedFile.size());
  }
  
  int dataLen = strlen(fragmentData);
  if (dataLen >= SD_LINE_BUFFER_SIZE) dataLen = SD_LINE_BUFFER_SIZE - 1;
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
  
  Serial.print(F("Frag #"));
  Serial.print(fragmentNumber);
  Serial.print(F(": "));
  Serial.print(pairsProcessed);
  Serial.println(F(" pairs"));
  
  return true;
}

bool SdUtils::finalizePresetFragmented(int cameraId, int presetId, int totalFragments) {
  if (!_presetFragmentedInProgress) {
    Serial.println(F("ERROR: No preset being created"));
    return false;
  }
  
  if (_presetFragmentedFile) {
    _presetFragmentedFile.flush();
    _presetFragmentedFile.close();
  }
  
  if (!tryLockSD(2000)) {
    Serial.println(F("ERROR: Could not access SD"));
    return false;
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
