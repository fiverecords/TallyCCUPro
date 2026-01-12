/*
 * webserver.cpp
 * Web server for control and configuration
 * 
 * TallyCCU Pro V3.0
 */

#include "WebServer.h"
#include "CCUState.h"
#include "CCUBroadcast.h"
#include <SdFat.h>
#include <avr/wdt.h>

#define SD SdUtils::getSd()

// ============================================================
// STATIC VARIABLE INITIALIZATION
// ============================================================

EthernetServer WebServer::_server(HTTP_PORT);
bool WebServer::_shouldReturnPresetValues = false;
int WebServer::_presetCameraIdToReturn = 0;
int WebServer::_presetIdToReturn = 0;

// Static buffers
char WebServer::_requestBuffer[WEB_REQUEST_BUFFER_SIZE];
char WebServer::_queryBuffer[WEB_QUERY_BUFFER_SIZE];
char WebServer::_keyBuffer[WEB_KEY_BUFFER_SIZE];
char WebServer::_valueBuffer[WEB_VALUE_BUFFER_SIZE];
char WebServer::_lineBuffer[WEB_LINE_BUFFER_SIZE];
char WebServer::_tempBuffer[WEB_TEMP_BUFFER_SIZE];

// ============================================================
// HELPER FUNCTIONS
// ============================================================

int WebServer::findChar(const char* str, char c, int start) {
  if (!str) return -1;
  for (int i = start; str[i] != '\0'; i++) {
    if (str[i] == c) return i;
  }
  return -1;
}

int WebServer::parseIntAt(const char* str, int start, int end) {
  if (!str || start < 0 || end <= start) return 0;
  char temp[16];
  int len = end - start;
  if (len > 15) len = 15;
  strncpy(temp, str + start, len);
  temp[len] = '\0';
  return atoi(temp);
}

bool WebServer::startsWithConst(const char* str, const char* prefix) {
  if (!str || !prefix) return false;
  return strncmp(str, prefix, strlen(prefix)) == 0;
}

void WebServer::extractSubstring(const char* src, char* dest, int start, int end, int maxLen) {
  if (!src || !dest || start < 0 || end <= start) {
    if (dest) dest[0] = '\0';
    return;
  }
  int len = end - start;
  if (len >= maxLen) len = maxLen - 1;
  strncpy(dest, src + start, len);
  dest[len] = '\0';
}

bool WebServer::isNumeric(const char* str) {
  if (!str || str[0] == '\0') return false;
  bool hasDecimal = false;
  int i = 0;
  if (str[0] == '-') i = 1;
  for (; str[i] != '\0'; i++) {
    if (str[i] == '.') {
      if (hasDecimal) return false;
      hasDecimal = true;
      continue;
    }
    if (!isdigit(str[i])) return false;
  }
  return true;
}

void WebServer::urlDecodeInPlace(char* str) {
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

// ============================================================
// INITIALIZATION
// ============================================================

bool WebServer::begin() {
  _server.begin();
  Serial.print(F("Web server on port "));
  Serial.println(HTTP_PORT);
  return true;
}

// ============================================================
// PROCESS REQUESTS - MAIN ENTRY POINT
// ============================================================

void WebServer::processRequests() {
  wdt_reset();
  
  EthernetClient client = _server.available();
  if (!client) return;
  
  wdt_reset();
  
  // Read first line of request
  int bufferIndex = 0;
  unsigned long startTime = millis();
  bool lineComplete = false;
  
  while (client.connected() && bufferIndex < WEB_REQUEST_BUFFER_SIZE - 1 && !lineComplete) {
    if (client.available()) {
      char ch = client.read();
      _requestBuffer[bufferIndex++] = ch;
      if (ch == '\n') lineComplete = true;
    }
    if (millis() - startTime > 5000) {
      Serial.println(F("Timeout reading request"));
      break;
    }
  }
  _requestBuffer[bufferIndex] = '\0';
  
  wdt_reset();
  
  // Detect request type
  if (strncmp(_requestBuffer, "POST ", 5) == 0) {
    processPOSTRequest(_requestBuffer, client);
  } else if (strncmp(_requestBuffer, "GET ", 4) == 0) {
    // Read and discard GET headers
    while (client.connected() && client.available()) {
      client.read();
    }
    processGETRequest(_requestBuffer, client);
  } else {
    client.println(F("HTTP/1.1 501 Not Implemented"));
    client.println(F("Connection: close"));
    client.println();
  }
  
  delay(1);
  client.flush();
  client.stop();
}

// ============================================================
// PROCESS POST - FOR SAVING PRESETS
// ============================================================

void WebServer::processPOSTRequest(const char* reqLine, EthernetClient &client) {
  Serial.println(F("[POST] Received"));
  
  // Check if savePreset
  if (strstr(reqLine, "POST /savePreset") != NULL) {
    handleSavePresetPOST(client);
    return;
  }
  
  // File upload
  if (strstr(reqLine, "POST /upload") != NULL) {
    handleUploadFile(client);
    return;
  }
  
  // Unrecognized POST
  client.println(F("HTTP/1.1 404 Not Found"));
  client.println(F("Connection: close"));
  client.println();
}

// ============================================================
// SAVE PRESET VIA POST - STREAMING WITH URL DECODE
// ============================================================

void WebServer::handleSavePresetPOST(EthernetClient &client) {
  Serial.println(F("[POST] savePreset"));
  
  // Read headers to get Content-Length
  int contentLength = 0;
  char headerLine[64];
  
  while (client.connected()) {
    int len = 0;
    unsigned long headerStart = millis();
    
    while (client.connected() && len < 63) {
      if (client.available()) {
        char c = client.read();
        if (c == '\n') break;
        if (c != '\r') headerLine[len++] = c;
      }
      if (millis() - headerStart > 2000) break;
    }
    headerLine[len] = '\0';
    
    if (len == 0) break;
    
    if (strncasecmp(headerLine, "Content-Length:", 15) == 0) {
      contentLength = atoi(headerLine + 15);
    }
    
    wdt_reset();
  }
  
  Serial.print(F("Content-Length: "));
  Serial.println(contentLength);
  
  if (contentLength <= 0) {
    sendJSONResponse(client, false, "No content");
    return;
  }
  
  // Read body header until "data="
  char headerBuf[96];
  int headerLen = 0;
  int bytesRead = 0;
  bool foundData = false;
  
  unsigned long bodyStart = millis();
  while (client.connected() && headerLen < 95 && bytesRead < contentLength && !foundData) {
    if (client.available()) {
      char c = client.read();
      bytesRead++;
      headerBuf[headerLen++] = c;
      
      if (headerLen >= 5 && strncmp(headerBuf + headerLen - 5, "data=", 5) == 0) {
        foundData = true;
      }
    }
    if (millis() - bodyStart > 3000) break;
    wdt_reset();
  }
  headerBuf[headerLen] = '\0';
  
  // Decode header
  urlDecodeInPlace(headerBuf);
  
  // Parse cameraId, presetId, name
  int cameraId = 0;
  int presetId = 0;
  char presetName[32] = "Preset";
  
  char* camPtr = strstr(headerBuf, "cameraId=");
  if (camPtr) cameraId = atoi(camPtr + 9);
  
  char* presetPtr = strstr(headerBuf, "presetId=");
  if (presetPtr) presetId = atoi(presetPtr + 9);
  
  char* namePtr = strstr(headerBuf, "name=");
  if (namePtr) {
    namePtr += 5;
    int i = 0;
    while (namePtr[i] && namePtr[i] != '&' && i < 31) {
      presetName[i] = namePtr[i];
      i++;
    }
    presetName[i] = '\0';
  }
  
  Serial.print(F("Cam:")); Serial.print(cameraId);
  Serial.print(F(" P:")); Serial.print(presetId);
  Serial.print(F(" N:")); Serial.println(presetName);
  
  if (!foundData || cameraId < 1 || cameraId > 8 || presetId < 0 || presetId > 9) {
    // Discard rest of body
    while (bytesRead < contentLength && client.connected()) {
      if (client.available()) { client.read(); bytesRead++; }
      wdt_reset();
    }
    sendJSONResponse(client, false, "Invalid params");
    return;
  }
  
  // Create file
  char filename[24];
  snprintf(filename, sizeof(filename), "preset_%d_%d.dat", cameraId, presetId);
  
  if (SD.exists(filename)) {
    SD.remove(filename);
    delay(30);
  }
  
  File presetFile = SD.open(filename, FILE_WRITE);
  if (!presetFile) {
    while (bytesRead < contentLength && client.connected()) {
      if (client.available()) { client.read(); bytesRead++; }
      wdt_reset();
    }
    sendJSONResponse(client, false, "File error");
    return;
  }
  
  // Metadata
  presetFile.print(F("cameraId:"));
  presetFile.println(cameraId);
  presetFile.print(F("presetId:"));
  presetFile.println(presetId);
  presetFile.print(F("name:"));
  presetFile.println(presetName);
  
  // Read data with real-time URL decode
  int paramCount = 0;
  char paramBuf[72];
  int paramLen = 0;
  
  unsigned long dataStart = millis();
  
  while (bytesRead < contentLength && client.connected()) {
    if (client.available()) {
      char c = client.read();
      bytesRead++;
      
      // URL decode en tiempo real
      if (c == '%' && (contentLength - bytesRead) >= 2) {
        unsigned long hexWait = millis();
        while (client.connected() && client.available() < 2 && (millis() - hexWait) < 100) {
        }
        if (client.available() >= 2) {
          char h1 = client.read();
          char h2 = client.read();
          bytesRead += 2;
          
          char hex[3] = {h1, h2, '\0'};
          c = (char)strtol(hex, NULL, 16);
        }
      } else if (c == '+') {
        c = ' ';
      }
      
      // Process decoded character
      if (c == ';') {
        paramBuf[paramLen] = '\0';
        if (paramLen > 0) {
          presetFile.println(paramBuf);
          paramCount++;
          
          if (paramCount % 15 == 0) {
            presetFile.flush();
            wdt_reset();
          }
        }
        paramLen = 0;
      } else if (paramLen < 71) {
        paramBuf[paramLen++] = c;
      }
    }
    
    if (millis() - dataStart > 15000) {
      Serial.println(F("Data timeout"));
      break;
    }
    
    if (bytesRead % 150 == 0) wdt_reset();
  }
  
  // Last parameter
  if (paramLen > 0) {
    paramBuf[paramLen] = '\0';
    presetFile.println(paramBuf);
    paramCount++;
  }
  
  presetFile.flush();
  presetFile.close();
  
  Serial.print(F("OK: "));
  Serial.print(paramCount);
  Serial.println(F(" params"));
  
  // Notify TCP clients that preset was saved
  CCUBroadcast::sendPresetSaved(cameraId, presetId, presetName);
  
  sendJSONResponse(client, true, "OK", paramCount);
}

// ============================================================
// SEND JSON RESPONSE
// ============================================================

void WebServer::sendJSONResponse(EthernetClient &client, bool success, const char* message, int paramCount) {
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Access-Control-Allow-Origin: *"));
  client.println(F("Connection: close"));
  client.println();
  
  client.print(F("{\"success\":"));
  client.print(success ? F("true") : F("false"));
  client.print(F(",\"message\":\""));
  client.print(message);
  client.print(F("\""));
  if (paramCount >= 0) {
    client.print(F(",\"paramCount\":"));
    client.print(paramCount);
  }
  client.println(F("}"));
}

// ============================================================
// PROCESS GET - MAIN FUNCTIONALITY
// ============================================================

void WebServer::processGETRequest(const char* reqLine, EthernetClient &client) {
  
  // Reboot
  if (strncmp(reqLine, "GET /reboot", 11) == 0) {
    Serial.println(F("Reboot requested"));
    client.println(F("HTTP/1.1 200 OK"));
    client.println(F("Content-Type: text/plain"));
    client.println(F("Connection: close"));
    client.println();
    client.println(F("Rebooting..."));
    delay(100);
    void(*resetFunc)(void) = 0;
    resetFunc();
    return;
  }

  // getOverrides
  if (strncmp(reqLine, "GET /?getOverrides", 18) == 0) {
    handleGetOverrides(client);
    return;
  }
  
  // getVmixConnect - vMix connection status (con IP)
  if (strncmp(reqLine, "GET /?getVmixConnect", 20) == 0) {
    byte vmixIP[4];
    VmixConnector::getVmixIP(vmixIP);
    
    client.println(F("HTTP/1.1 200 OK"));
    client.println(F("Content-Type: application/json"));
    client.println(F("Access-Control-Allow-Origin: *"));
    client.println(F("Connection: close"));
    client.println();
    client.print(F("{\"vmixConnectEnabled\":"));
    client.print(VmixConnector::getConnectEnabled() ? F("true") : F("false"));
    client.print(F(",\"vmixConnected\":"));
    client.print(VmixConnector::isConnected() ? F("true") : F("false"));
    client.print(F(",\"vmixIP\":\""));
    client.print(vmixIP[0]); client.print('.');
    client.print(vmixIP[1]); client.print('.');
    client.print(vmixIP[2]); client.print('.');
    client.print(vmixIP[3]);
    client.println(F("\"}"));
    return;
  }
  
  // getParams - Endpoint for web synchronization
  if (strncmp(reqLine, "GET /?getParams=", 16) == 0) {
    int cameraId = atoi(reqLine + 16);
    if (cameraId < 1 || cameraId > MAX_CAMERAS) {
      cameraId = CCUControl::getActiveCamera();
    }
    
    CCUState::writeStateAsJSON(client, cameraId);
    return;
  }
  
  // ============================================================
  // SD FILE MANAGEMENT
  // ============================================================
  
  // listFiles - List files on SD
  if (strncmp(reqLine, "GET /?listFiles", 15) == 0) {
    handleListFiles(client);
    return;
  }
  
  // download - Download file
  if (strncmp(reqLine, "GET /?download=", 15) == 0) {
    int spacePos = findChar(reqLine, ' ', 15);
    if (spacePos > 15) {
      extractSubstring(reqLine, _tempBuffer, 15, spacePos, WEB_TEMP_BUFFER_SIZE);
      urlDecodeInPlace(_tempBuffer);
      handleDownloadFile(client, _tempBuffer);
    } else {
      send404(client);
    }
    return;
  }
  
  // deleteFile - Delete file
  if (strncmp(reqLine, "GET /?deleteFile=", 17) == 0) {
    int spacePos = findChar(reqLine, ' ', 17);
    if (spacePos > 17) {
      extractSubstring(reqLine, _tempBuffer, 17, spacePos, WEB_TEMP_BUFFER_SIZE);
      urlDecodeInPlace(_tempBuffer);
      handleDeleteFile(client, _tempBuffer);
    } else {
      send404(client);
    }
    return;
  }
  
  // renameFile - Rename file: /?renameFile=oldname&to=newname
  if (strncmp(reqLine, "GET /?renameFile=", 17) == 0) {
    int spacePos = findChar(reqLine, ' ', 17);
    if (spacePos > 17) {
      extractSubstring(reqLine, _queryBuffer, 17, spacePos, WEB_QUERY_BUFFER_SIZE);
      urlDecodeInPlace(_queryBuffer);
      
      char* toPtr = strstr(_queryBuffer, "&to=");
      if (toPtr) {
        *toPtr = '\0';
        toPtr += 4;
        handleRenameFile(client, _queryBuffer, toPtr);
      } else {
        sendJSONResponse(client, false, "Missing newname");
      }
    } else {
      send404(client);
    }
    return;
  }
  
  // fileExists - Check if file exists
  if (strncmp(reqLine, "GET /?fileExists=", 17) == 0) {
    int spacePos = findChar(reqLine, ' ', 17);
    if (spacePos > 17) {
      extractSubstring(reqLine, _tempBuffer, 17, spacePos, WEB_TEMP_BUFFER_SIZE);
      urlDecodeInPlace(_tempBuffer);
      
      client.println(F("HTTP/1.1 200 OK"));
      client.println(F("Content-Type: application/json"));
      client.println(F("Access-Control-Allow-Origin: *"));
      client.println(F("Connection: close"));
      client.println();
      client.print(F("{\"exists\":"));
      client.print(SD.exists(_tempBuffer) ? F("true") : F("false"));
      client.println(F("}"));
    } else {
      send404(client);
    }
    return;
  }
  
  // listPresets
  if (strncmp(reqLine, "GET /?listPresets", 17) == 0) {
    Serial.println(F("Listing presets"));
    listPresetsAsJSON(client);
    return;
  }
  
  // Main page
  if (strncmp(reqLine, "GET / ", 6) == 0 || strncmp(reqLine, "GET /index.html", 15) == 0) {
    SdUtils::serveFile(client, "index.html");
    return;
  }
  
  // Tally page
  if (strncmp(reqLine, "GET /tally.html", 15) == 0) {
    SdUtils::serveFile(client, "tally.html");
    return;
  }
  
  // SD management page (direct access)
  if (strncmp(reqLine, "GET /sd", 7) == 0) {
    SdUtils::serveFile(client, "sdcard.html");
    return;
  }

  // Static files
  if (strncmp(reqLine, "GET /", 5) == 0 && strncmp(reqLine, "GET /?", 6) != 0) {
    int spacePos = findChar(reqLine, ' ', 5);
    if (spacePos > 5) {
      extractSubstring(reqLine, _tempBuffer, 5, spacePos, WEB_TEMP_BUFFER_SIZE);
      if (SdUtils::serveFile(client, _tempBuffer)) {
        return;
      }
    }
  }

  // Find query string
  const char* queryStart = strstr(reqLine, "GET /?");
  if (!queryStart) {
    send404(client);
    return;
  }
  
  int queryStartPos = 6;
  int spacePos = findChar(reqLine, ' ', queryStartPos);
  if (spacePos < 0) {
    send404(client);
    return;
  }
  
  extractSubstring(reqLine, _queryBuffer, queryStartPos, spacePos, WEB_QUERY_BUFFER_SIZE);
  
  // loadPreset con returnValues
  if (strncmp(_queryBuffer, "loadPreset=", 11) == 0) {
    char* returnValuesPos = strstr(_queryBuffer, "&returnValues=");
    bool returnValues = (returnValuesPos != NULL);
    
    if (returnValuesPos) *returnValuesPos = '\0';
    
    int commaPos = findChar(_queryBuffer, ',', 11);
    if (commaPos > 0) {
      int cameraId = parseIntAt(_queryBuffer, 11, commaPos);
      int presetIdx = parseIntAt(_queryBuffer, commaPos + 1, strlen(_queryBuffer));
      
      Serial.print(F("Cargando preset cam:"));
      Serial.print(cameraId);
      Serial.print(F(" preset:"));
      Serial.println(presetIdx);
      
      bool success = SdUtils::applyPreset(cameraId, presetIdx);
      
      if (success && returnValues) {
        sendJSONPresetValues(client, cameraId, presetIdx);
        return;
      }
    }
    
    client.println(F("HTTP/1.1 200 OK"));
    client.println(F("Content-Type: text/plain"));
    client.println(F("Connection: close"));
    client.println();
    client.println(F("OK"));
    return;
  }
  
  // Parse normal parameters
  int idx = 0;
  int queryLen = strlen(_queryBuffer);
  
  while (idx < queryLen) {
    int ampPos = findChar(_queryBuffer, '&', idx);
    int pairEnd = (ampPos < 0) ? queryLen : ampPos;
    
    int eqPos = findChar(_queryBuffer, '=', idx);
    if (eqPos > 0 && eqPos < pairEnd) {
      extractSubstring(_queryBuffer, _keyBuffer, idx, eqPos, WEB_KEY_BUFFER_SIZE);
      extractSubstring(_queryBuffer, _valueBuffer, eqPos + 1, pairEnd, WEB_VALUE_BUFFER_SIZE);
      handleParam(_keyBuffer, _valueBuffer, client);
    }
    
    idx = (ampPos < 0) ? queryLen : ampPos + 1;
  }

  // Respuesta por defecto
  if (_shouldReturnPresetValues) {
    sendJSONPresetValues(client, _presetCameraIdToReturn, _presetIdToReturn);
    _shouldReturnPresetValues = false;
  } else {
    client.println(F("HTTP/1.1 200 OK"));
    client.println(F("Content-Type: text/plain"));
    client.println(F("Connection: close"));
    client.println();
    client.println(F("OK"));
  }
}

// ============================================================
// ENVIAR JSON CON VALORES DEL PRESET
// ============================================================

void WebServer::sendJSONPresetValues(EthernetClient &client, int cameraId, int presetId) {
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Access-Control-Allow-Origin: *"));
  client.println(F("Connection: close"));
  client.println();
  
  char filename[32];
  snprintf(filename, sizeof(filename), "preset_%d_%d.dat", cameraId, presetId);
  
  File presetFile = SD.open(filename);
  if (!presetFile) {
    client.println(F("{\"parameters\":{}}"));
    return;
  }
  
  client.print(F("{\"parameters\":{"));
  bool firstParam = true;
  
  while (presetFile.available()) {
    int len = 0;
    while (presetFile.available() && len < WEB_LINE_BUFFER_SIZE - 1) {
      char c = presetFile.read();
      if (c == '\n' || c == '\r') break;
      _lineBuffer[len++] = c;
    }
    _lineBuffer[len] = '\0';
    
    if (len == 0) continue;
    
    int colonPos = findChar(_lineBuffer, ':', 0);
    if (colonPos <= 0) continue;
    
    extractSubstring(_lineBuffer, _keyBuffer, 0, colonPos, WEB_KEY_BUFFER_SIZE);
    extractSubstring(_lineBuffer, _valueBuffer, colonPos + 1, len, WEB_VALUE_BUFFER_SIZE);
    urlDecodeInPlace(_valueBuffer);
    
    if (strcmp(_keyBuffer, "cameraId") == 0 || strcmp(_keyBuffer, "presetId") == 0) {
      continue;
    }
    
    if (!firstParam) client.print(F(","));
    firstParam = false;
    
    client.print(F("\""));
    client.print(_keyBuffer);
    client.print(F("\":"));
    
    if (strchr(_valueBuffer, ',') != NULL) {
      client.print(F("["));
      char* token = strtok(_valueBuffer, ",");
      bool firstVal = true;
      while (token != NULL) {
        if (!firstVal) client.print(F(","));
        firstVal = false;
        client.print(token);
        token = strtok(NULL, ",");
      }
      client.print(F("]"));
    } else {
      if (isNumeric(_valueBuffer)) {
        client.print(_valueBuffer);
      } else {
        client.print(F("\""));
        client.print(_valueBuffer);
        client.print(F("\""));
      }
    }
  }
  
  client.print(F("}}"));
  presetFile.close();
}

// ============================================================
// HANDLE GET PARAMETERS
// ============================================================

void WebServer::handleParam(const char* key, const char* value, EthernetClient &client) {
  wdt_reset();
  
  // vmixConnect
  if (strcmp(key, "vmixConnect") == 0) {
    VmixConnector::setConnectEnabled(value[0] == '1');
    return;
  }

  // vmixIP
  if (strcmp(key, "vmixIP") == 0) {
    int parts[4] = {0, 0, 0, 0};
    int partIdx = 0;
    int start = 0;
    int len = strlen(value);
    
    for (int i = 0; i <= len && partIdx < 4; i++) {
      if (value[i] == '.' || value[i] == '\0') {
        parts[partIdx++] = parseIntAt(value, start, i);
        start = i + 1;
      }
    }
    
    if (partIdx == 4) {
      snprintf(_tempBuffer, WEB_TEMP_BUFFER_SIZE, "%d.%d.%d.%d", parts[0], parts[1], parts[2], parts[3]);
      changeVMixIP(_tempBuffer);
    }
    return;
  }

  // Tally map - tallyCamXInput
  if (strncmp(key, "tallyCam", 8) == 0 && strstr(key, "Input") != NULL) {
    const char* numStart = key + 8;
    const char* inputPos = strstr(key, "Input");
    if (inputPos > numStart) {
      int len = inputPos - numStart;
      char numBuf[8];
      strncpy(numBuf, numStart, len);
      numBuf[len] = '\0';
      int idx = atoi(numBuf);
      int v = atoi(value);
      if (idx >= 0 && idx < MAXTALLIES) {
        TallyManager::setMapping(idx, v, 0);
      }
    }
    return;
  }

  // Camera map - camXID
  if (strncmp(key, "cam", 3) == 0 && strstr(key, "ID") != NULL) {
    const char* numStart = key + 3;
    const char* idPos = strstr(key, "ID");
    if (idPos > numStart) {
      int len = idPos - numStart;
      char numBuf[8];
      strncpy(numBuf, numStart, len);
      numBuf[len] = '\0';
      int idx = atoi(numBuf);
      int v = atoi(value);
      if (idx >= 0 && idx < MAXTALLIES) {
        TallyManager::setMapping(idx, 0, v);
      }
    }
    return;
  }

  // cameraId
  if (strcmp(key, "cameraId") == 0) {
    int cameraId = atoi(value);
    if (cameraId >= 1 && cameraId <= MAX_CAMERAS) {
      CCUControl::setActiveCamera(cameraId);
    }
    return;
  }

  // Overrides
  if (strcmp(key, "overrideTally") == 0) {
    TallyManager::setOverride(value[0] == '1');
    return;
  }

  if (strcmp(key, "overrideCCU") == 0) {
    CCUControl::setOverride(value[0] == '1');
    return;
  }

  // CCU params por defecto
  strncpy(_valueBuffer, value, WEB_VALUE_BUFFER_SIZE - 1);
  _valueBuffer[WEB_VALUE_BUFFER_SIZE - 1] = '\0';
  urlDecodeInPlace(_valueBuffer);
  CCUControl::applyParameterByKey(key, String(_valueBuffer));
}

// ============================================================
// FUNCIONES HTTP AUXILIARES
// ============================================================

void WebServer::handleGetOverrides(EthernetClient &client) {
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Access-Control-Allow-Origin: *"));
  client.println(F("Connection: close"));
  client.println();
  client.print(F("{\"overrideTally\":"));
  client.print(TallyManager::getOverride() ? F("true") : F("false"));
  client.print(F(",\"overrideCCU\":"));
  client.print(CCUControl::getOverride() ? F("true") : F("false"));
  client.println(F("}"));
}

void WebServer::listPresetsAsJSON(EthernetClient &client) {
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Access-Control-Allow-Origin: *"));
  client.println(F("Connection: close"));
  client.println();
  SdUtils::writePresetListToClient(client);
}

void WebServer::send404(EthernetClient &client) {
  client.println(F("HTTP/1.1 404 Not Found"));
  client.println(F("Content-Type: text/plain"));
  client.println(F("Connection: close"));
  client.println();
  client.println(F("404 Not Found"));
}

// ============================================================
// SD FILE MANAGEMENT
// ============================================================

void WebServer::handleListFiles(EthernetClient &client) {
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Access-Control-Allow-Origin: *"));
  client.println(F("Connection: close"));
  client.println();
  
  client.print(F("{\"files\":["));
  
  File root = SD.open("/");
  if (root) {
    bool first = true;
    File entry;
    char fileName[32];
    while ((entry = root.openNextFile())) {
      if (!entry.isDirectory()) {
        entry.getName(fileName, sizeof(fileName));
        
        // Hide system files
        if (strcmp(fileName, "index.html") == 0 ||
            strcmp(fileName, "tally.html") == 0 ||
            strcmp(fileName, "sdcard.html") == 0) {
          entry.close();
          continue;
        }
        
        if (!first) client.print(F(","));
        first = false;
        
        client.print(F("{\"name\":\""));
        client.print(fileName);
        client.print(F("\",\"size\":"));
        client.print((unsigned long)entry.size());
        client.print(F("}"));
      }
      entry.close();
      wdt_reset();
    }
    root.close();
  }
  
  client.println(F("]}"));
}

void WebServer::handleDownloadFile(EthernetClient &client, const char* filename) {
  File file = SD.open(filename);
  if (!file) {
    send404(client);
    return;
  }
  
  client.println(F("HTTP/1.1 200 OK"));
  client.print(F("Content-Disposition: attachment; filename=\""));
  client.print(filename);
  client.println(F("\""));
  client.println(F("Content-Type: application/octet-stream"));
  client.print(F("Content-Length: "));
  client.println((unsigned long)file.size());
  client.println(F("Connection: close"));
  client.println();
  
  // Send content
  uint8_t buf[64];
  while (file.available()) {
    int len = file.read(buf, sizeof(buf));
    if (len > 0) {
      client.write(buf, len);
    }
    wdt_reset();
  }
  
  file.close();
}

void WebServer::handleDeleteFile(EthernetClient &client, const char* filename) {
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Access-Control-Allow-Origin: *"));
  client.println(F("Connection: close"));
  client.println();
  
  // Protect system files
  if (strcmp(filename, "index.html") == 0 ||
      strcmp(filename, "tally.html") == 0 ||
      strcmp(filename, "sdcard.html") == 0) {
    client.println(F("{\"success\":false,\"error\":\"protected\"}"));
    return;
  }
  
  bool success = false;
  if (SD.exists(filename)) {
    success = SD.remove(filename);
  }
  
  client.print(F("{\"success\":"));
  client.print(success ? F("true") : F("false"));
  client.println(F("}"));
}

void WebServer::handleRenameFile(EthernetClient &client, const char* oldName, const char* newName) {
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Access-Control-Allow-Origin: *"));
  client.println(F("Connection: close"));
  client.println();
  
  bool success = false;
  if (SD.exists(oldName) && !SD.exists(newName)) {
    success = SD.rename(oldName, newName);
  }
  
  client.print(F("{\"success\":"));
  client.print(success ? F("true") : F("false"));
  client.println(F("}"));
}

void WebServer::handleUploadFile(EthernetClient &client) {
  Serial.println(F("[POST] upload"));
  
  // Read headers HTTP
  long contentLength = 0;
  char boundary[48] = "";
  char headerLine[96];
  
  while (client.connected()) {
    int len = 0;
    unsigned long headerStart = millis();
    
    while (client.connected() && len < 95) {
      if (client.available()) {
        char c = client.read();
        if (c == '\n') break;
        if (c != '\r') headerLine[len++] = c;
      }
      if (millis() - headerStart > 2000) break;
    }
    headerLine[len] = '\0';
    
    if (len == 0) break;
    
    if (strncasecmp(headerLine, "Content-Length:", 15) == 0) {
      contentLength = atol(headerLine + 15);
    }
    
    char* bndPtr = strstr(headerLine, "boundary=");
    if (bndPtr) {
      bndPtr += 9;
      if (*bndPtr == '"') bndPtr++;
      int i = 0;
      while (bndPtr[i] && bndPtr[i] != '"' && bndPtr[i] != '\r' && bndPtr[i] != ';' && i < 46) {
        boundary[i] = bndPtr[i];
        i++;
      }
      boundary[i] = '\0';
    }
    
    wdt_reset();
  }
  
  if (contentLength <= 0 || boundary[0] == '\0') {
    while (client.available()) client.read();
    sendJSONResponse(client, false, "Bad request");
    return;
  }
  
  // Build end pattern: \r\n--boundary
  char endPattern[56] = "\r\n--";
  strcat(endPattern, boundary);
  int patternLen = strlen(endPattern);
  
  // Buffer for pattern detection
  char* delayBuf = (char*)malloc(patternLen + 1);
  if (!delayBuf) {
    while (client.available()) client.read();
    sendJSONResponse(client, false, "Memory error");
    return;
  }
  int delayBufLen = 0;
  
  char filename[32] = "";
  long bytesRead = 0;
  bool inFileContent = false;
  File uploadFile;
  
  unsigned long startTime = millis();
  
  while (bytesRead < contentLength && client.connected() && (millis() - startTime) < 60000) {
    if (!client.available()) {
      delay(1);
      continue;
    }
    
    if (!inFileContent) {
      // Read part headers
      int len = 0;
      while (client.available() && len < 95 && bytesRead < contentLength) {
        char c = client.read();
        bytesRead++;
        if (c == '\n') break;
        if (c != '\r') headerLine[len++] = c;
      }
      headerLine[len] = '\0';
      
      char* fnPtr = strstr(headerLine, "filename=\"");
      if (fnPtr) {
        fnPtr += 10;
        int i = 0;
        while (fnPtr[i] && fnPtr[i] != '"' && i < 31) {
          filename[i] = fnPtr[i];
          i++;
        }
        filename[i] = '\0';
      }
      
      if (len == 0 && filename[0] != '\0') {
        inFileContent = true;
        delayBufLen = 0;
        
        if (SD.exists(filename)) {
          SD.remove(filename);
          delay(20);
        }
        uploadFile = SD.open(filename, FILE_WRITE);
        if (!uploadFile) {
          free(delayBuf);
          while (bytesRead < contentLength && client.available()) {
            client.read(); bytesRead++;
          }
          sendJSONResponse(client, false, "SD error");
          return;
        }
      }
    } else {
      char c = client.read();
      bytesRead++;
      
      // Add to delay buffer
      delayBuf[delayBufLen++] = c;
      
      // Check if buffer contains end pattern
      if (delayBufLen >= patternLen) {
        delayBuf[delayBufLen] = '\0';
        if (strncmp(delayBuf + delayBufLen - patternLen, endPattern, patternLen) == 0) {
          // Found! Write all except pattern
          if (delayBufLen > patternLen) {
            uploadFile.write((uint8_t*)delayBuf, delayBufLen - patternLen);
          }
          break;
        }
        
        // Write first byte and shift buffer
        uploadFile.write((uint8_t)delayBuf[0]);
        memmove(delayBuf, delayBuf + 1, delayBufLen - 1);
        delayBufLen--;
      }
      
      if (bytesRead % 512 == 0) {
        uploadFile.flush();
        wdt_reset();
      }
    }
    
    wdt_reset();
  }
  
  free(delayBuf);
  
  if (uploadFile) {
    uploadFile.flush();
    uploadFile.close();
  }
  
  sendJSONResponse(client, filename[0] != '\0', filename[0] != '\0' ? filename : "No file");
}