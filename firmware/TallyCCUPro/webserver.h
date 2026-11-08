/*
 * WebServer.h
 * Web server for control and configuration
 * Version 3.8
 * 
 * RAM optimized with static buffers
 * Includes SSE (Server-Sent Events) for real-time sync
 * Includes Safe Mode diagnostics
 */

#ifndef WEBSERVER_H
#define WEBSERVER_H

#include "Configuration.h"
#include "Network.h"
#include "TallyManager.h"
#include "CCUControl.h"
#include "SdUtils.h"
#include "VmixConnector.h"
#include <SdFat.h>

// External declaration for vMix IP change
extern void changeVMixIP(const char* ip);

// Static buffer sizes - optimized
#define WEB_REQUEST_BUFFER_SIZE 192
#define WEB_QUERY_BUFFER_SIZE 192
#define WEB_KEY_BUFFER_SIZE 32
#define WEB_VALUE_BUFFER_SIZE 96
#define WEB_LINE_BUFFER_SIZE 96
#define WEB_TEMP_BUFFER_SIZE 48

// SSE timeout (30 seconds without activity)
#define SSE_TIMEOUT 30000

class WebServer {
  public:
    static bool begin();
    static void processRequests();
    
    // SSE: Send event to connected client (called from CCUBroadcast)
    static void sendSSEEvent(int cameraId, const char* paramKey, const char* value);
    static void sendSSEPresetLoaded(int cameraId, int presetId, const char* presetName);
    static void sendSSEPresetSaved(int cameraId, int presetId, const char* presetName);
    static void sendSSERequestSync();  // Request web to send its state
    static bool hasSSEClient();
    
    // JSON utility - print string with " and \ escaped
    static void printJsonSafe(Print &out, const char* str);

  private:
    static EthernetServer _server;
    
    // SSE client
    static EthernetClient _sseClient;
    static unsigned long _sseLastActivity;

    // Result of one GET parameter; the first error of a query decides the response
    enum ParamResult : uint8_t {
      PARAM_OK = 0,
      PARAM_ERR_KEY,         // 400 malformed or unknown key
      PARAM_ERR_LENGTH,      // 400 key or value too long for the buffers
      PARAM_ERR_CAMERA,      // 400 cameraId out of 1..MAX_CAMERAS
      PARAM_ERR_SLOT,        // 400 tally slot out of 0..MAXTALLIES-1
      PARAM_ERR_INPUT,       // 400 vMix input out of 1..MAX_VMIX_INPUTS
      PARAM_ERR_MAP_CAMERA,  // 400 tally camera out of 1..MAX_CAMERAS
      PARAM_ERR_VALUE,       // 400 value rejected
      PARAM_ERR_SAFE_MODE    // 503 CCU not available
    };

    // Per-request state of a GET with parameters
    struct ParamContext {
      int8_t camera;               // cameraId of this request: 0 none, -1 invalid
      bool badRequest;             // a 400 happened: tally map edits are dropped
      byte mapInput[MAXTALLIES];   // staged tally map edits (0 = unchanged)
      byte mapCam[MAXTALLIES];
    };

    // Static buffers
    static char _requestBuffer[WEB_REQUEST_BUFFER_SIZE];
    static char _queryBuffer[WEB_QUERY_BUFFER_SIZE];
    static char _keyBuffer[WEB_KEY_BUFFER_SIZE];
    static char _valueBuffer[WEB_VALUE_BUFFER_SIZE];
    static char _lineBuffer[WEB_LINE_BUFFER_SIZE];
    static char _tempBuffer[WEB_TEMP_BUFFER_SIZE];
    
    // Process different request types
    static void processGETRequest(const char* reqLine, EthernetClient &client);
    static void processPOSTRequest(const char* reqLine, EthernetClient &client);
    
    // Handle preset save via POST
    static void handleSavePresetPOST(EthernetClient &client);
    
    // Handle sync state from web via POST
    static void handleSyncStatePOST(EthernetClient &client);
    
    // Handle GET parameters (value is URL-decoded in place)
    static ParamResult handleParam(const char* key, char* value, ParamContext &ctx);

    // /?loadPreset=cam,idx[&returnValues=1]
    static void handleLoadPreset(EthernetClient &client, char* args);

    // HTTP responses
    static void sendHeaders(EthernetClient &client, int httpCode, bool json);
    static void sendParamResult(EthernetClient &client, ParamResult result);
    static void handleGetOverrides(EthernetClient &client);
    static void listPresetsAsJSON(EthernetClient &client);
    static void send404(EthernetClient &client);
    static void sendJSONPresetValues(EthernetClient &client, int cameraId, int presetId);
    static void sendJSONResponse(EthernetClient &client, int httpCode, bool success, const __FlashStringHelper* message, int paramCount = -1);
    static void sendJSONResponse(EthernetClient &client, int httpCode, bool success, const char* message, int paramCount = -1);
    static void sendJSONTail(EthernetClient &client, int paramCount);
    
    // SSE handling
    static void handleSSEConnection(EthernetClient &client);
    static void processSSEClient();
    
    // SD file management
    static void handleListFiles(EthernetClient &client);
    static void handleDownloadFile(EthernetClient &client, const char* filename);
    static void handleDeleteFile(EthernetClient &client, const char* filename);
    static void handleRenameFile(EthernetClient &client, const char* oldName, const char* newName);
    static void handleUploadFile(EthernetClient &client);
    
    // Safe Mode handlers
    static void handleSafeModeStatus(EthernetClient &client);
    static void handleSafeModeExit(EthernetClient &client);
    
    // Utilities
    static bool isNumeric(const char* str);
    static void urlDecodeInPlace(char* str);
    static int findChar(const char* str, char c, int start);
    static bool startsWithConst(const char* str, const char* prefix);
    static void extractSubstring(const char* src, char* dest, int start, int end, int maxLen);
};

#endif // WEBSERVER_H
