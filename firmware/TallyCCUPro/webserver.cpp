/*
 * webserver.cpp
 * Web server for control and configuration
 * 
 * TallyCCU Pro V3.7.1
 */

#include "webserver.h"
#include "CCUBroadcast.h"
#include "SafeMode.h"
#include <SdFat.h>
#include <avr/wdt.h>

#define SD SdUtils::getSd()

// ============================================================
// STATIC VARIABLE INITIALIZATION
// ============================================================

EthernetServer WebServer::_server(HTTP_PORT);

// SSE client
EthernetClient WebServer::_sseClient;
unsigned long WebServer::_sseLastActivity = 0;

// Static buffers
char WebServer::_requestBuffer[WEB_REQUEST_BUFFER_SIZE];
char WebServer::_queryBuffer[WEB_QUERY_BUFFER_SIZE];
char WebServer::_keyBuffer[WEB_KEY_BUFFER_SIZE];
char WebServer::_valueBuffer[WEB_VALUE_BUFFER_SIZE];
char WebServer::_lineBuffer[WEB_LINE_BUFFER_SIZE];
char WebServer::_tempBuffer[WEB_TEMP_BUFFER_SIZE];

// ============================================================
// STRICT PARSERS (no RAM literals, no truncation)
// ============================================================

// Parses "<prefix><digits><suffix>" with the suffix at the very end.
// Returns the number (saturated at LONG_MAX) or -1 if str does not match.
static long parseIndexed(const char* str, PGM_P prefix, PGM_P suffix) {
  size_t n = strlen_P(prefix);
  if (strncmp_P(str, prefix, n) != 0) return -1;
  str += n;
  if (!isdigit((unsigned char)*str)) return -1;   // no sign, space or empty number
  char* end;
  long v = strtol(str, &end, 10);
  if (strcmp_P(end, suffix) != 0) return -1;
  return v;
}

// tallyCam<N>Input -> N (isInput = true), cam<N>ID -> N (isInput = false), else -1
static long parseTallyMapKey(const char* key, bool &isInput) {
  long slot = parseIndexed(key, PSTR("tallyCam"), PSTR("Input"));
  isInput = (slot >= 0);
  if (!isInput) slot = parseIndexed(key, PSTR("cam"), PSTR("ID"));
  return slot;
}

// "a.b.c.d" with four decimal octets 0..255 and nothing else
static bool parseIPv4(const char* s, byte out[4]) {
  for (byte i = 0; i < 4; i++) {
    if (!isdigit((unsigned char)*s)) return false;
    char* end;
    long v = strtol(s, &end, 10);
    if (v > 255 || *end != (i < 3 ? '.' : '\0')) return false;
    out[i] = (byte)v;
    s = end + 1;
  }
  return true;
}

// "0" or "1" only: anything else is rejected instead of meaning false
static int8_t parseFlag(const char* s) {
  if ((s[0] == '0' || s[0] == '1') && s[1] == '\0') return s[0] - '0';
  return -1;
}

// Cuts s to size-1 bytes, drops an incomplete trailing UTF-8 sequence and
// replaces control characters (they would break the file and JSON)
static void clampPresetName(char* s, size_t size) {
  size_t len = strlen(s);
  if (len >= size) len = size - 1;
  size_t lead = len;
  while (lead > 0 && ((uint8_t)s[lead - 1] & 0xC0) == 0x80) lead--;
  if (lead > 0 && (uint8_t)s[lead - 1] >= 0xC0) {
    uint8_t c = s[lead - 1];
    size_t need = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
    if (len - (lead - 1) < need) len = lead - 1;
  }
  s[len] = '\0';
  for (size_t i = 0; i < len; i++) {
    if ((uint8_t)s[i] < 0x20 || s[i] == 0x7F) s[i] = ' ';
  }
}

// Discards the rest of a request body (bounded wait)
static void drainBody(EthernetClient &client, long remaining) {
  unsigned long start = millis();
  while (remaining > 0 && client.connected() && millis() - start < 2000) {
    if (client.available()) {
      client.read();
      remaining--;
    }
    wdt_reset();
  }
}

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

// Print a string to any Print target (client, Serial) with JSON escaping
void WebServer::printJsonSafe(Print &out, const char* str) {
  if (!str) return;
  for (int i = 0; str[i] != '\0'; i++) {
    char c = str[i];
    if (c == '"' || c == '\\') {
      out.print('\\');
    }
    out.print(c);
  }
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
  
  // Process existing SSE client
  processSSEClient();
  
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
  
  // Check for SSE endpoint FIRST (before consuming headers)
  if (strncmp(_requestBuffer, "GET /events", 11) == 0) {
    // Read and discard remaining headers quickly
    unsigned long headerStart = millis();
    while (client.connected() && (millis() - headerStart) < 1000) {
      if (client.available()) {
        client.read();
        headerStart = millis(); // Reset timeout on activity
      } else {
        delay(1);
      }
      // Check for end of headers (empty line)
      if (!client.available()) break;
    }
    handleSSEConnection(client);
    return;  // Don't close client!
  }
  
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
#if DEBUG_HOT
  Serial.println(F("[POST] Received"));
#endif

  // Safe mode exit
  if (strstr(reqLine, "POST /safemode-exit") != NULL) {
    handleSafeModeExit(client);
    return;
  }
  
  // Check if savePreset
  if (strstr(reqLine, "POST /savePreset") != NULL) {
    handleSavePresetPOST(client);
    return;
  }
  
  // Sync state from web to TCP clients
  if (strstr(reqLine, "POST /syncState") != NULL) {
    handleSyncStatePOST(client);
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
#if DEBUG_HOT
  Serial.println(F("[POST] savePreset"));
#endif
  
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
  
#if DEBUG_HOT
  Serial.print(F("Content-Length: "));
  Serial.println(contentLength);
#endif
  
  if (contentLength <= 0) {
    sendJSONResponse(client, 400, false, F("No content"));
    return;
  }
  
  // Body "cameraId=X&presetId=Y&name=Z&data=...": parse field by field
  // up to data= (no fixed prefix limit; a long name is cut, not rejected)
  long cameraId = -1;
  long presetId = -1;
  char presetName[MAX_PRESET_NAME_LENGTH];
  strcpy_P(presetName, PSTR("Preset"));
  
  int bytesRead = 0;
  int keyLen = 0;
  int valLen = 0;
  bool inValue = false;
  bool valOverflow = false;
  bool foundData = false;
  
  unsigned long bodyStart = millis();
  while (!foundData && bytesRead < contentLength && client.connected()) {
    if (millis() - bodyStart > 3000) break;
    wdt_reset();
    if (!client.available()) continue;
    
    char c = client.read();
    bytesRead++;
    
    if (!inValue) {
      if (c == '=') {
        _keyBuffer[keyLen] = '\0';
        if (strcmp_P(_keyBuffer, PSTR("data")) == 0) {
          foundData = true;
        } else {
          inValue = true;
          valLen = 0;
          valOverflow = false;
        }
      } else if (c == '&') {
        keyLen = 0;
      } else if (keyLen < WEB_KEY_BUFFER_SIZE - 1) {
        _keyBuffer[keyLen++] = c;
      }
      continue;
    }
    
    if (c != '&') {
      if (valLen < WEB_VALUE_BUFFER_SIZE - 1) _valueBuffer[valLen++] = c;
      else valOverflow = true;
      if (bytesRead < contentLength) continue;
    }
    
    // End of a field value
    _valueBuffer[valLen] = '\0';
    if (strcmp_P(_keyBuffer, PSTR("cameraId")) == 0) {
      cameraId = valOverflow ? -1 : parseIndexed(_valueBuffer, PSTR(""), PSTR(""));
    } else if (strcmp_P(_keyBuffer, PSTR("presetId")) == 0) {
      presetId = valOverflow ? -1 : parseIndexed(_valueBuffer, PSTR(""), PSTR(""));
    } else if (strcmp_P(_keyBuffer, PSTR("name")) == 0) {
      // 95 encoded bytes decode to >= 31 bytes: overflow only loses the tail
      urlDecodeInPlace(_valueBuffer);
      clampPresetName(_valueBuffer, sizeof(presetName));
      strcpy(presetName, _valueBuffer);
    }
    inValue = false;
    keyLen = 0;
  }
  
#if DEBUG_HOT
  Serial.print(F("Cam:")); Serial.print(cameraId);
  Serial.print(F(" P:")); Serial.print(presetId);
  Serial.print(F(" N:")); Serial.println(presetName);
#endif
  
  if (!foundData || cameraId < 1 || cameraId > MAX_CAMERAS || presetId < 0 || presetId >= MAX_PRESETS) {
    drainBody(client, contentLength - bytesRead);
    sendJSONResponse(client, 400, false, F("Invalid params"));
    return;
  }
  
  // Create file (name kept in _tempBuffer: removed again if the save fails)
  char* filename = _tempBuffer;
  SdUtils::getPresetFilename(filename, WEB_TEMP_BUFFER_SIZE, cameraId, presetId);
  
  if (SD.exists(filename)) {
    if (!SD.remove(filename)) {
      drainBody(client, contentLength - bytesRead);
      sendJSONResponse(client, 500, false, F("File error"));
      return;
    }
    delay(30);
  }
  
  File presetFile = SD.open(filename, FILE_WRITE);
  if (!presetFile) {
    drainBody(client, contentLength - bytesRead);
    sendJSONResponse(client, 500, false, F("File error"));
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
  int paramLen = 0;
  bool paramOverflow = false;
  int skipped = 0;   // over-long params: dropped whole, never cut
  
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
        _lineBuffer[paramLen] = '\0';
        if (paramOverflow) {
          skipped++;
        } else if (paramLen > 0) {
          presetFile.println(_lineBuffer);
          paramCount++;
          
          if (paramCount % 15 == 0) {
            presetFile.flush();
            wdt_reset();
          }
        }
        paramLen = 0;
        paramOverflow = false;
      } else if (paramLen < WEB_LINE_BUFFER_SIZE - 1) {
        _lineBuffer[paramLen++] = c;
      } else {
        paramOverflow = true;
      }
    }
    
    if (millis() - dataStart > 15000) {
      Serial.println(F("Data timeout"));
      break;
    }
    
    wdt_reset();
  }
  
  // Last parameter
  if (paramOverflow) {
    skipped++;
  } else if (paramLen > 0) {
    _lineBuffer[paramLen] = '\0';
    presetFile.println(_lineBuffer);
    paramCount++;
  }
  
  bool writeOk = !presetFile.getWriteError();
  writeOk = presetFile.close() && writeOk;   // close() syncs to the card
  
  if (!writeOk || bytesRead < contentLength) {
    // A partial preset would load only some values: do not keep it
    SD.remove(filename);
    drainBody(client, contentLength - bytesRead);
    Serial.print(F("Preset save failed: "));
    Serial.println(filename);
    if (!writeOk) {
      sendJSONResponse(client, 500, false, F("Write error"));
    } else {
      sendJSONResponse(client, 400, false, F("Incomplete data"));
    }
    return;
  }
  
#if DEBUG_HOT
  Serial.print(F("OK: "));
  Serial.print(paramCount);
  Serial.println(F(" params"));
#endif
  
  // Notify TCP clients that preset was saved
  CCUBroadcast::sendPresetSaved(cameraId, presetId, presetName);
  
  // Notify SSE client (web)
  sendSSEPresetSaved(cameraId, presetId, presetName);
  
  if (skipped > 0) {
    sendHeaders(client, 200, true);
    client.print(F("{\"success\":true,\"message\":\"OK, "));
    client.print(skipped);
    client.print(F(" skipped (too long)"));
    sendJSONTail(client, paramCount);
  } else {
    sendJSONResponse(client, 200, true, F("OK"), paramCount);
  }
}

// ============================================================
// SYNC STATE FROM WEB TO TCP CLIENTS
// ============================================================

void WebServer::handleSyncStatePOST(EthernetClient &client) {
#if DEBUG_HOT
  Serial.println(F("[POST] syncState"));
#endif
  
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
  
  if (contentLength <= 0) {
    sendJSONResponse(client, 200, false, F("No content"));
    return;
  }
  
  // Read body line by line: format "cam:key:value\n"
  int paramCount = 0;
  int bytesRead = 0;
  unsigned long bodyStart = millis();
  
  while (bytesRead < contentLength && client.connected() && (millis() - bodyStart) < 10000) {
    if (!client.available()) {
      delay(1);
      continue;
    }
    
    // Read one line
    int len = 0;
    while (client.available() && bytesRead < contentLength && len < WEB_LINE_BUFFER_SIZE - 1) {
      char c = client.read();
      bytesRead++;
      if (c == '\n') break;
      if (c != '\r') _lineBuffer[len++] = c;
    }
    _lineBuffer[len] = '\0';
    
    if (len == 0) continue;
    
    // Parse "cam:key:value"
    char* firstColon = strchr(_lineBuffer, ':');
    if (!firstColon) continue;
    
    *firstColon = '\0';
    int cameraId = atoi(_lineBuffer);
    
    char* secondColon = strchr(firstColon + 1, ':');
    if (!secondColon) continue;
    
    *secondColon = '\0';
    const char* paramKey = firstColon + 1;
    const char* value = secondColon + 1;
    
    if (cameraId >= 1 && cameraId <= MAX_CAMERAS && strlen(paramKey) > 0) {
      // Broadcast to TCP clients (Companion)
      CCUBroadcast::sendParamChange(cameraId, paramKey, value);
      paramCount++;
    }
    
    wdt_reset();
  }
  
#if DEBUG_HOT
  Serial.print(F("[syncState] Forwarded "));
  Serial.print(paramCount);
  Serial.println(F(" params to TCP"));
#endif
  
  sendJSONResponse(client, 200, true, F("OK"), paramCount);
}

// ============================================================
// SEND RESPONSES
// ============================================================

// Status line and headers (JSON responses also carry CORS)
void WebServer::sendHeaders(EthernetClient &client, int httpCode, bool json) {
  client.print(F("HTTP/1.1 "));
  client.print(httpCode);
  switch (httpCode) {
    case 200: client.println(F(" OK")); break;
    case 400: client.println(F(" Bad Request")); break;
    case 404: client.println(F(" Not Found")); break;
    case 503: client.println(F(" Service Unavailable")); break;
    default:  client.println(F(" Internal Server Error")); break;
  }
  if (json) {
    client.println(F("Content-Type: application/json"));
    client.println(F("Access-Control-Allow-Origin: *"));
  } else {
    client.println(F("Content-Type: text/plain"));
  }
  client.println(F("Connection: close"));
  client.println();
}

// Plain-text answer to a GET with parameters: "OK" or "ERROR <reason>"
void WebServer::sendParamResult(EthernetClient &client, ParamResult result) {
  int code = 400;
  const __FlashStringHelper* msg;
  switch (result) {
    case PARAM_OK:             code = 200; msg = F("OK"); break;
    case PARAM_ERR_SAFE_MODE:  code = 503; msg = F("ERROR safe mode"); break;
    case PARAM_ERR_LENGTH:     msg = F("ERROR key or value too long"); break;
    case PARAM_ERR_CAMERA:     msg = F("ERROR bad cameraId"); break;
    case PARAM_ERR_SLOT:       msg = F("ERROR bad tally slot"); break;
    case PARAM_ERR_INPUT:      msg = F("ERROR bad vMix input"); break;
    case PARAM_ERR_MAP_CAMERA: msg = F("ERROR bad tally camera"); break;
    case PARAM_ERR_VALUE:      msg = F("ERROR bad value"); break;
    default:                   msg = F("ERROR unknown key"); break;
  }
  sendHeaders(client, code, false);
  client.println(msg);
}

// {"success":bool,"message":str[,"paramCount":n]}
void WebServer::sendJSONResponse(EthernetClient &client, int httpCode, bool success, const __FlashStringHelper* message, int paramCount) {
  sendHeaders(client, httpCode, true);
  client.print(success ? F("{\"success\":true,\"message\":\"") : F("{\"success\":false,\"message\":\""));
  client.print(message);
  sendJSONTail(client, paramCount);
}

void WebServer::sendJSONResponse(EthernetClient &client, int httpCode, bool success, const char* message, int paramCount) {
  sendHeaders(client, httpCode, true);
  client.print(success ? F("{\"success\":true,\"message\":\"") : F("{\"success\":false,\"message\":\""));
  printJsonSafe(client, message);
  sendJSONTail(client, paramCount);
}

void WebServer::sendJSONTail(EthernetClient &client, int paramCount) {
  client.print('"');
  if (paramCount >= 0) {
    client.print(F(",\"paramCount\":"));
    client.print(paramCount);
  }
  client.println('}');
}

// ============================================================
// PROCESS GET - MAIN FUNCTIONALITY
// ============================================================

void WebServer::processGETRequest(const char* reqLine, EthernetClient &client) {
  
  // ============================================================
  // SAFE MODE HANDLING - CHECK FIRST
  // ============================================================
  
  // Safe mode status API (always available)
  if (strncmp(reqLine, "GET /safemode-status", 20) == 0) {
    handleSafeModeStatus(client);
    return;
  }
  
  // In safe mode, redirect main pages to diagnostic page
  if (SafeMode::isActive()) {
    if (strncmp(reqLine, "GET / ", 6) == 0 || 
        strncmp(reqLine, "GET /index", 10) == 0 ||
        strncmp(reqLine, "GET /ccu", 8) == 0) {
      SdUtils::serveFile(client, "safemode.html");
      return;
    }
  }
  
  // ============================================================
  // NORMAL REQUEST HANDLING
  // ============================================================
  
  // Reboot
  if (strncmp(reqLine, "GET /reboot", 11) == 0) {
    Serial.println(F("Reboot requested"));
    client.println(F("HTTP/1.1 200 OK"));
    client.println(F("Content-Type: text/plain"));
    client.println(F("Connection: close"));
    client.println();
    client.println(F("Rebooting..."));
    client.flush();
    client.stop();
    delay(100);
    SafeMode::restart();   // intentional: not counted as a boot failure
    return;
  }

  // getOverrides
  if (strncmp(reqLine, "GET /?getOverrides", 18) == 0) {
    handleGetOverrides(client);
    return;
  }
  
  // getTallyMap - Return current tally input->camera mappings
  if (strncmp(reqLine, "GET /?getTallyMap", 17) == 0) {
    client.println(F("HTTP/1.1 200 OK"));
    client.println(F("Content-Type: application/json"));
    client.println(F("Access-Control-Allow-Origin: *"));
    client.println(F("Connection: close"));
    client.println();
    client.print(F("{\"mappings\":["));
    byte inp, cam;
    for (byte i = 0; i < MAXTALLIES; i++) {
      TallyManager::getMapping(i, inp, cam);
      if (i > 0) client.print(',');
      client.print(F("{\"input\":"));
      client.print(inp);
      client.print(F(",\"camId\":"));
      client.print(cam);
      client.print('}');
    }
    client.println(F("]}"));
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
  
  // getParams - Legacy endpoint (SSE is now primary sync method)
  // Returns empty - web uses SSE for real-time state
  if (strncmp(reqLine, "GET /?getParams=", 16) == 0) {
    int cameraId = atoi(reqLine + 16);
    if (cameraId < 1 || cameraId > MAX_CAMERAS) {
      cameraId = CCUControl::getActiveCamera();
    }
    
    client.println(F("HTTP/1.1 200 OK"));
    client.println(F("Content-Type: application/json"));
    client.println(F("Access-Control-Allow-Origin: *"));
    client.println(F("Connection: close"));
    client.println();
    client.print(F("{\"cameraId\":"));
    client.print(cameraId);
    client.println(F(",\"paramCount\":0,\"params\":{}}"));
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
        sendJSONResponse(client, 200, false, F("Missing newname"));
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
#if DEBUG_HOT
    Serial.println(F("Listing presets"));
#endif
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
  if (strncmp_P(_queryBuffer, PSTR("loadPreset="), 11) == 0) {
    handleLoadPreset(client, _queryBuffer + 11);
    return;
  }
  
  // Parse normal parameters. Every valid pair is applied; the first
  // error decides the answer (200 OK, 400 or 503 "ERROR <reason>").
  ParamContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ParamResult result = PARAM_OK;
  int idx = 0;
  int queryLen = strlen(_queryBuffer);
  
  while (idx < queryLen) {
    int ampPos = findChar(_queryBuffer, '&', idx);
    int pairEnd = (ampPos < 0) ? queryLen : ampPos;
    
    if (pairEnd > idx) {   // skip empty pairs ("&&", trailing '&')
      int eqPos = findChar(_queryBuffer, '=', idx);
      ParamResult r;
      if (eqPos <= idx || eqPos >= pairEnd) {
        r = PARAM_ERR_KEY;      // empty key or no '='
      } else if (eqPos - idx >= WEB_KEY_BUFFER_SIZE || pairEnd - eqPos - 1 >= WEB_VALUE_BUFFER_SIZE) {
        r = PARAM_ERR_LENGTH;   // never act on a truncated key or value
      } else {
        extractSubstring(_queryBuffer, _keyBuffer, idx, eqPos, WEB_KEY_BUFFER_SIZE);
        extractSubstring(_queryBuffer, _valueBuffer, eqPos + 1, pairEnd, WEB_VALUE_BUFFER_SIZE);
        r = handleParam(_keyBuffer, _valueBuffer, ctx);
      }
      if (r != PARAM_OK) {
        if (r != PARAM_ERR_SAFE_MODE) ctx.badRequest = true;
        if (result == PARAM_OK) result = r;
      }
    }
    
    idx = (ampPos < 0) ? queryLen : ampPos + 1;
  }
  
  // Tally map edits are all-or-nothing: after a 400 the map is untouched
  if (!ctx.badRequest) {
    for (byte s = 0; s < MAXTALLIES; s++) {
      if (ctx.mapInput[s] || ctx.mapCam[s]) {
        TallyManager::setMapping(s, ctx.mapInput[s], ctx.mapCam[s]);
      }
    }
  }
  
  sendParamResult(client, result);
}

// ============================================================
// LOAD PRESET
// ============================================================

void WebServer::handleLoadPreset(EthernetClient &client, char* args) {
  char* rv = strstr_P(args, PSTR("&returnValues="));
  bool returnValues = (rv != NULL && rv[14] == '1');
  char* amp = strchr(args, '&');
  if (amp) *amp = '\0';
  
  // "cam,idx", both strict decimal
  long cameraId = -1;
  long presetId = -1;
  char* comma = strchr(args, ',');
  if (comma) {
    *comma = '\0';
    cameraId = parseIndexed(args, PSTR(""), PSTR(""));
    presetId = parseIndexed(comma + 1, PSTR(""), PSTR(""));
  }
  
  if (cameraId < 1 || cameraId > MAX_CAMERAS || presetId < 0 || presetId >= MAX_PRESETS) {
    sendJSONResponse(client, 400, false, F("Invalid params"));
    return;
  }
  
  if (!CCUControl::isReady()) {
    sendJSONResponse(client, 503, false, F("safe mode"));
    return;
  }
  
  SdUtils::getPresetFilename(_tempBuffer, WEB_TEMP_BUFFER_SIZE, cameraId, presetId);
  if (!SdUtils::fileExists(_tempBuffer)) {
    sendJSONResponse(client, 404, false, F("Preset not found"));
    return;
  }
  
#if DEBUG_HOT
  Serial.print(F("Loading preset cam:"));
  Serial.print(cameraId);
  Serial.print(F(" preset:"));
  Serial.println(presetId);
#endif
  
  if (!SdUtils::applyPreset(cameraId, presetId)) {
    sendJSONResponse(client, 500, false, F("Preset load error"));
    return;
  }
  
  if (returnValues) {
    sendJSONPresetValues(client, cameraId, presetId);
  } else {
    sendJSONResponse(client, 200, true, F("OK"));
  }
}

// ============================================================
// ENVIAR JSON CON VALORES DEL PRESET
// ============================================================

// Only called after a successful applyPreset: always "success":true
void WebServer::sendJSONPresetValues(EthernetClient &client, int cameraId, int presetId) {
  sendHeaders(client, 200, true);
  
  SdUtils::getPresetFilename(_tempBuffer, WEB_TEMP_BUFFER_SIZE, cameraId, presetId);
  
  File presetFile = SD.open(_tempBuffer);
  if (!presetFile) {
    client.println(F("{\"success\":true,\"parameters\":{}}"));
    return;
  }
  
  client.print(F("{\"success\":true,\"parameters\":{"));
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
    
    if (strcmp_P(_keyBuffer, PSTR("cameraId")) == 0 || strcmp_P(_keyBuffer, PSTR("presetId")) == 0) {
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
        printJsonSafe(client, _valueBuffer);
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

WebServer::ParamResult WebServer::handleParam(const char* key, char* value, ParamContext &ctx) {
  wdt_reset();
  urlDecodeInPlace(value);
  
  // vmixConnect
  if (strcmp_P(key, PSTR("vmixConnect")) == 0) {
    int8_t on = parseFlag(value);
    if (on < 0) return PARAM_ERR_VALUE;
    VmixConnector::setConnectEnabled(on == 1);
    return PARAM_OK;
  }

  // vmixIP
  if (strcmp_P(key, PSTR("vmixIP")) == 0) {
    byte ip[4];
    if (!parseIPv4(value, ip)) return PARAM_ERR_VALUE;
    snprintf_P(_tempBuffer, WEB_TEMP_BUFFER_SIZE, PSTR("%u.%u.%u.%u"), ip[0], ip[1], ip[2], ip[3]);
    changeVMixIP(_tempBuffer);
    return PARAM_OK;
  }

  // cameraId: target of the CCU params that follow
  if (strcmp_P(key, PSTR("cameraId")) == 0) {
    long cam = parseIndexed(value, PSTR(""), PSTR(""));
    if (cam < 1 || cam > MAX_CAMERAS) {
      ctx.camera = -1;   // following CCU params must not go to another camera
      return PARAM_ERR_CAMERA;
    }
    ctx.camera = (int8_t)cam;
    CCUControl::setActiveCamera(cam);
    return PARAM_OK;
  }

  // Overrides
  if (strcmp_P(key, PSTR("overrideTally")) == 0) {
    int8_t on = parseFlag(value);
    if (on < 0) return PARAM_ERR_VALUE;
    TallyManager::setOverride(on == 1);
    return PARAM_OK;
  }

  if (strcmp_P(key, PSTR("overrideCCU")) == 0) {
    if (!CCUControl::isReady()) return PARAM_ERR_SAFE_MODE;
    int8_t on = parseFlag(value);
    if (on < 0) return PARAM_ERR_VALUE;
    CCUControl::setOverride(on == 1);
    return PARAM_OK;
  }

  // Tally map: tallyCam<slot>Input=<vMix input>, cam<slot>ID=<camera>.
  // Staged in ctx; processGETRequest commits it if the query had no 400.
  bool isInput;
  long slot = parseTallyMapKey(key, isInput);
  if (slot >= 0) {
    if (slot >= MAXTALLIES) return PARAM_ERR_SLOT;
    long v = parseIndexed(value, PSTR(""), PSTR(""));
    if (isInput) {
      if (v < 1 || v > MAX_VMIX_INPUTS) return PARAM_ERR_INPUT;
      ctx.mapInput[slot] = (byte)v;
    } else {
      if (v < 1 || v > MAX_CAMERAS) return PARAM_ERR_MAP_CAMERA;
      ctx.mapCam[slot] = (byte)v;
    }
    return PARAM_OK;
  }

  // Anything else is a CCU parameter
  if (ctx.camera < 0) return PARAM_ERR_CAMERA;
  if (!CCUControl::isReady()) {
    return CCUControl::findParamDefByKey(key) ? PARAM_ERR_SAFE_MODE : PARAM_ERR_KEY;
  }
  if (CCUControl::applyParameterByKey(key, value, ctx.camera > 0 ? ctx.camera : -1)) {
    return PARAM_OK;
  }
  return CCUControl::findParamDefByKey(key) ? PARAM_ERR_VALUE : PARAM_ERR_KEY;
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
  client.print(F(",\"ccuAvailable\":"));
  client.print(CCUControl::isReady() ? F("true") : F("false"));
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
            strcmp(fileName, "sdcard.html") == 0 ||
            strcmp(fileName, "safemode.html") == 0) {
          entry.close();
          continue;
        }
        
        if (!first) client.print(F(","));
        first = false;
        
        client.print(F("{\"name\":\""));
        printJsonSafe(client, fileName);
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
      strcmp(filename, "sdcard.html") == 0 ||
      strcmp(filename, "safemode.html") == 0) {
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
#if DEBUG_HOT
  Serial.println(F("[POST] upload"));
#endif
  
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
    sendJSONResponse(client, 200, false, F("Bad request"));
    return;
  }
  
  // Build end pattern: \r\n--boundary
  char endPattern[56] = "\r\n--";
  strcat(endPattern, boundary);
  int patternLen = strlen(endPattern);
  
  // Static buffer for pattern detection (max boundary=48 + "\r\n--"=4 + null=1 = 53)
  char delayBuf[56];
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
          while (bytesRead < contentLength && client.available()) {
            client.read(); bytesRead++;
          }
          sendJSONResponse(client, 200, false, F("SD error"));
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
  
  if (uploadFile) {
    uploadFile.flush();
    uploadFile.close();
  }
  
  if (filename[0] != '\0') {
    sendJSONResponse(client, 200, true, filename);
  } else {
    sendJSONResponse(client, 200, false, F("No file"));
  }
}

// ============================================================
// SAFE MODE HANDLERS
// ============================================================

void WebServer::handleSafeModeStatus(EthernetClient &client) {
  // Send headers
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Cache-Control: no-cache"));
  client.println(F("Access-Control-Allow-Origin: *"));
  client.println(F("Connection: close"));
  client.println();
  
  // Calculate free RAM
  extern int __heap_start, *__brkval;
  int v;
  int freeRam = (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
  
  // Build JSON response
  client.print(F("{"));
  client.print(F("\"safeMode\":"));
  client.print(SafeMode::isActive() ? F("true") : F("false"));
  client.print(F(",\"resetCount\":"));
  client.print(SafeMode::getResetCount());
  client.print(F(",\"resetReason\":\""));
  client.print(SafeMode::getResetReasonString());
  client.print(F("\",\"uptime\":"));
  client.print(SafeMode::getUptime());
  client.print(F(",\"freeRam\":"));
  client.print(freeRam);
  client.print(F(",\"ip\":\""));
  client.print(NetworkManager::getLocalIP());
  client.println(F("\"}"));
}

void WebServer::handleSafeModeExit(EthernetClient &client) {
  // Send response first
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Access-Control-Allow-Origin: *"));
  client.println(F("Connection: close"));
  client.println();
  client.println(F("{\"success\":true,\"message\":\"Restarting...\"}"));
  
  // Close connection
  delay(100);
  client.flush();
  client.stop();
  
  // Small delay to ensure response is sent
  delay(200);
  
  // Exit safe mode (this triggers a restart)
  SafeMode::exitSafeMode();
}

// ============================================================
// SSE (SERVER-SENT EVENTS) HANDLING
// ============================================================

void WebServer::handleSSEConnection(EthernetClient &client) {
  // Close existing SSE client if any
  if (_sseClient && _sseClient.connected()) {
    Serial.println(F("[SSE] Closing old client"));
    _sseClient.stop();
  }
  
  Serial.println(F("[SSE] New client connected"));
  
  // Send SSE headers
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: text/event-stream"));
  client.println(F("Cache-Control: no-cache"));
  client.println(F("Access-Control-Allow-Origin: *"));
  client.println(F("Connection: keep-alive"));
  client.println();
  
  // Send initial connection event
  client.println(F("event: connected"));
  client.println(F("data: {\"status\":\"ok\"}"));
  client.println();
  
  // Store client reference
  _sseClient = client;
  _sseLastActivity = millis();
  
  // Request state sync from TCP clients (Companion)
  CCUBroadcast::requestSync();
}

void WebServer::processSSEClient() {
  if (!_sseClient) return;
  
  // Check if still connected
  if (!_sseClient.connected()) {
    Serial.println(F("[SSE] Client disconnected"));
    _sseClient.stop();
    _sseClient = EthernetClient();
    return;
  }
  
  // Check timeout
  if (millis() - _sseLastActivity > SSE_TIMEOUT) {
    // Send keepalive comment
    _sseClient.println(F(": keepalive"));
    _sseClient.println();
    _sseLastActivity = millis();
  }
  
  // Discard any incoming data (browser might send something)
  while (_sseClient.available()) {
    _sseClient.read();
  }
}

bool WebServer::hasSSEClient() {
  return _sseClient && _sseClient.connected();
}

void WebServer::sendSSEEvent(int cameraId, const char* paramKey, const char* value) {
  if (!_sseClient || !_sseClient.connected()) return;
  
  // Format: data: {"cam":1,"key":"gain_db","val":"6"}
  _sseClient.print(F("data: {\"cam\":"));
  _sseClient.print(cameraId);
  _sseClient.print(F(",\"key\":\""));
  _sseClient.print(paramKey);
  _sseClient.print(F("\",\"val\":\""));
  printJsonSafe(_sseClient, value);
  _sseClient.println(F("\"}"));
  _sseClient.println();  // Empty line marks end of event
  
  _sseLastActivity = millis();
}

void WebServer::sendSSEPresetLoaded(int cameraId, int presetId, const char* presetName) {
  if (!_sseClient || !_sseClient.connected()) return;
  
  // Format: data: {"type":"presetLoaded","cam":1,"preset":0,"name":"Default"}
  _sseClient.print(F("data: {\"type\":\"presetLoaded\",\"cam\":"));
  _sseClient.print(cameraId);
  _sseClient.print(F(",\"preset\":"));
  _sseClient.print(presetId);
  _sseClient.print(F(",\"name\":\""));
  printJsonSafe(_sseClient, presetName ? presetName : "");
  _sseClient.println(F("\"}"));
  _sseClient.println();
  
  _sseLastActivity = millis();
}

void WebServer::sendSSEPresetSaved(int cameraId, int presetId, const char* presetName) {
  if (!_sseClient || !_sseClient.connected()) return;
  
  // Format: data: {"type":"presetSaved","cam":1,"preset":0,"name":"My Preset"}
  _sseClient.print(F("data: {\"type\":\"presetSaved\",\"cam\":"));
  _sseClient.print(cameraId);
  _sseClient.print(F(",\"preset\":"));
  _sseClient.print(presetId);
  _sseClient.print(F(",\"name\":\""));
  printJsonSafe(_sseClient, presetName ? presetName : "");
  _sseClient.println(F("\"}"));
  _sseClient.println();
  
  _sseLastActivity = millis();
}

void WebServer::sendSSERequestSync() {
  if (!_sseClient || !_sseClient.connected()) return;
  
  // Request web client to send its cached state
  _sseClient.println(F("data: {\"type\":\"requestSync\"}"));
  _sseClient.println();
  
  _sseLastActivity = millis();
}
