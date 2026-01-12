/*
 * WebServer.h
 * Web server for control and configuration
 * Version 3.0
 * 
 * RAM optimized with static buffers
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

class WebServer {
  public:
    static bool begin();
    static void processRequests();

  private:
    static EthernetServer _server;
    
    // Variables for preset value return
    static bool _shouldReturnPresetValues;
    static int _presetCameraIdToReturn;
    static int _presetIdToReturn;
    
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
    
    // Handle GET parameters
    static void handleParam(const char* key, const char* value, EthernetClient &client);
    
    // HTTP responses
    static void handleGetOverrides(EthernetClient &client);
    static void listPresetsAsJSON(EthernetClient &client);
    static void send404(EthernetClient &client);
    static void sendJSONPresetValues(EthernetClient &client, int cameraId, int presetId);
    static void sendJSONResponse(EthernetClient &client, bool success, const char* message, int paramCount = -1);
    
    // SD file management
    static void handleListFiles(EthernetClient &client);
    static void handleDownloadFile(EthernetClient &client, const char* filename);
    static void handleDeleteFile(EthernetClient &client, const char* filename);
    static void handleRenameFile(EthernetClient &client, const char* oldName, const char* newName);
    static void handleUploadFile(EthernetClient &client);
    
    // Utilities
    static bool isNumeric(const char* str);
    static void urlDecodeInPlace(char* str);
    static int findChar(const char* str, char c, int start);
    static int parseIntAt(const char* str, int start, int end);
    static bool startsWithConst(const char* str, const char* prefix);
    static void extractSubstring(const char* src, char* dest, int start, int end, int maxLen);
};

#endif // WEBSERVER_H
