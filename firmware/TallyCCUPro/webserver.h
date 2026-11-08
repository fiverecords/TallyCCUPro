/*
 * WebServer.h
 * Web server for control and configuration
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
#include "NetUtils.h"
#include <SdFat.h>

// External declaration for vMix IP change
extern void changeVMixIP(const char* ip);

// Static buffer sizes - optimized
#define WEB_REQUEST_BUFFER_SIZE 192   // request line, then response/transfer buffer
#define WEB_KEY_BUFFER_SIZE 32
#define WEB_VALUE_BUFFER_SIZE 96
#define WEB_LINE_BUFFER_SIZE 96
#define WEB_TEMP_BUFFER_SIZE 48
#define WEB_SSE_BUFFER_SIZE 176       // one SSE event, sent all or nothing

// SD file manager names (list, download, delete, rename, upload,
// fileExists): decoded bytes, one limit for all (SD-06)
#define WEB_NAME_MAX (WEB_TEMP_BUFFER_SIZE - 1)

// HTTP time budgets (ms)
#define WEB_HEADER_MS 2000            // request line + headers, total
#define WEB_RESPONSE_MS 3000          // writing one response, total
#define WEB_CLOSE_MS 500              // graceful close after a response
#define WEB_BODY_MS 3000              // request body: base budget...
#define WEB_BODY_BYTES_PER_MS 8       // ...plus Content-Length at 8 KB/s
#define WEB_BODY_MAX_MS 60000UL
#define WEB_BODY_IDLE_MS 3000         // request body: no data this long aborts
#define WEB_PRESET_MAX_BODY 16384L    // savePreset body limit (a preset is ~3 KB)
#define WEB_SYNC_MAX_BODY 65536L      // syncState body limit (8 cameras)
#define WEB_IDLE_SOCKET_MS 5000       // port-80 socket without a request: closed
#define WEB_LISTENERS 2               // LISTEN sockets on port 80 while 3+ are free
#define WEB_MAX_PER_PASS 3            // requests served per processRequests()...
#define WEB_PASS_MS 25                // ...unless this much time has gone
#define WEB_POLL_MS 20                // long requests and waits: tally poll period
#define WEB_DEFER_MS 10000            // page/file request waiting for a transfer
                                      // slot (SD_XFERS busy): then a 503...
#define WEB_DEFER_MAX_MS (SD_SEND_MAX_MS + SD_SEND_FIN_MS + 2000)
                                      // ...or up to this (a slot is free by then)
#define WEB_DEFER_SPARE 1             // while this many sockets are still free
                                      // (listener re-armed: one for vMix)

// SSE
#define SSE_TIMEOUT 30000             // keepalive after 30 s without events
#define MAX_SSE_CLIENTS 2             // a new one replaces the oldest
#define SSE_MAX_DROPS 128             // events dropped in a row: close (a preset
                                      // load alone is ~50 back-to-back events)
#define SSE_STALL_MS 2000             // dropping events this long: close
#define SSE_TX_WAIT_MS 2              // grace for a busy TX before a drop
#define SSE_STATE_MIN_MS 100          // tally/state events at most this often

// PTZ / continuous zoom safety stop, only for requests with deadman=1:
// no refresh for PTZ_DEADMAN_MS stops the motion. While the server is busy
// (a long request, requests still waiting, and PTZ_DEADMAN_MS after that)
// a refresh may be held up, so only the longer limit applies.
#define PTZ_DEADMAN_MS 600
#define PTZ_DEADMAN_BUSY_MS 2400

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
    
    // JSON string contents: " and \ escaped, control characters as
    // \n, \r, \t or \u00XX (HTTP-10)
    static void printJsonSafe(Print &out, const char* str);
    static void printJsonSafe(Print &out, const char* str, size_t len);

    // For loops that serve one web request for long: polls the vMix tally,
    // the deadman and the port-80 listener, at most every WEB_POLL_MS.
    // File transfers are not moved on from here (SdUtils::pumpTransfers()).
    static void transferPoll();

  private:
    static EthernetServer _server;
    
    // SSE clients, oldest first. Only the socket index and the peer are
    // kept: the index is checked against them before every use.
    struct SseClient {
      uint8_t sock;
      uint8_t drops;                // events dropped in a row
      uint16_t rport;
      uint32_t rip;
      unsigned long dropSince;      // first drop of the current run
    };
    static SseClient _sse[MAX_SSE_CLIENTS];
    static uint8_t _sseCount;
    static unsigned long _sseLastActivity;
    static bool _sseSyncWanted;     // sseResync() asked for a REQUESTSYNC

    // Tally (UI-18) and global switches (UI-10) as last sent on the stream;
    // a stale bit sends them again (new or recovered client)
    static uint8_t _ssePgm, _ssePvw, _sseSwitches;
    static uint8_t _sseStale;
    static unsigned long _sseStateAt;

    // Port-80 sockets: when first seen connected (FIFO order, idle sweep)
    static uint16_t _sockSeenAt[MAX_SOCK_NUM];
    static uint8_t _sockSeenMask;

    // Deadman: armed cameras (bit cam-1), last refresh (8 ms ticks, wraps
    // after 524 s) and the end of the last busy period
    static uint8_t _dmPan, _dmZoom;
    static uint16_t _dmPanAt[MAX_CAMERAS], _dmZoomAt[MAX_CAMERAS];
    static unsigned long _dmBusyAt;
    static uint16_t _pollAt;        // last transferPoll() (ms, 16 bit)

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
      bool deadman;                // deadman=1
      uint8_t pan, zoom;           // motion applied: 0 none, 1 stop, 2 moving
      uint8_t panCam, zoomCam;
      byte mapInput[MAXTALLIES];   // staged tally map edits: 0 unchanged,
      byte mapCam[MAXTALLIES];     // MAP_CLEAR an explicit 0
    };

    // Headers of the request being served
    struct Request {
      long contentLength;          // -1 missing or invalid
      bool gzip;                   // Accept-Encoding: gzip
      bool boundary;               // multipart end pattern is in _valueBuffer
      bool csrf;                   // X-TallyCCU: 1 (SEC-01)
      bool crossSite;              // Sec-Fetch-Site other than same-origin/none
    };

    // Static buffers
    static char _requestBuffer[WEB_REQUEST_BUFFER_SIZE];
    static char _keyBuffer[WEB_KEY_BUFFER_SIZE];
    static char _valueBuffer[WEB_VALUE_BUFFER_SIZE];
    static char _lineBuffer[WEB_LINE_BUFFER_SIZE];
    static char _tempBuffer[WEB_TEMP_BUFFER_SIZE];
    static char _sseBuffer[WEB_SSE_BUFFER_SIZE];
    
    // Connection handling
    static uint8_t scanSockets();
    static void keepListening();
    static void serveClient(uint8_t sock);
    static void readHeaders(EthernetClient &client, unsigned long deadline, Request &req);
    static int readLinePolled(EthernetClient &client, NetUtils::LineReader &lr,
                              unsigned long deadline, long* remaining);

    // Process different request types
    static void processGETRequest(char* reqLine, EthernetClient &client, const Request &req);
    static void processPOSTRequest(const char* reqLine, EthernetClient &client, const Request &req);
    static void processQuery(char* query, EthernetClient &client, const Request &req);
    
    // Handle preset save via POST
    static void handleSavePresetPOST(EthernetClient &client, long contentLength);
    
    // Handle sync state from web via POST
    static void handleSyncStatePOST(EthernetClient &client, long contentLength);
    
    // Handle GET parameters (value is URL-decoded in place)
    static ParamResult handleParam(const char* key, char* value, ParamContext &ctx);

    // /?loadPreset=cam,idx[&returnValues=1]
    static void handleLoadPreset(EthernetClient &client, char* args);

    // Deadman timers
    static void armDeadman(bool zoom, uint8_t cam, bool arm);
    static void checkDeadman(bool busy);

    // HTTP responses (written through _requestBuffer)
    static void sendHeaders(Print &out, int httpCode, bool json);
    static void sendBareStatus(EthernetClient &client, int httpCode);
    static void sendForbidden(EthernetClient &client);
    static void sendFileResult(EthernetClient &client, int httpCode, PGM_P error);
    static void sendParamResult(EthernetClient &client, ParamResult result);
    static void handleGetOverrides(EthernetClient &client);
    static void handleGetTallyMap(EthernetClient &client);
    static void handleGetVmixConnect(EthernetClient &client);
    static void listPresetsAsJSON(EthernetClient &client);
    static void send404(EthernetClient &client);
    static void serveSdFile(EthernetClient &client, PGM_P filename, bool gzip);
    static void sendJSONPresetValues(EthernetClient &client, int cameraId, int presetId);
    static void sendJSONResponse(EthernetClient &client, int httpCode, bool success, const __FlashStringHelper* message, int paramCount = -1);
    static void sendJSONResponse(EthernetClient &client, int httpCode, bool success, const char* message, int paramCount = -1);
    static void sendJSONHead(Print &out, int httpCode, bool success);
    static void sendJSONTail(Print &out, int paramCount);
    static void sendJSONBool(EthernetClient &client, PGM_P key, bool value);
    
    // SSE handling
    static bool handleSSEConnection(EthernetClient &client);
    static void processSSEClients();
    static bool sseValid(uint8_t i);
    static void sseRemove(uint8_t i);
    static void sseBroadcast(const ClientBufferPrint &event);
    static void sseResync(EthernetClient &c, SseClient &s);
    static void sendSSEState();
    
    // SD file management (names checked by the caller)
    static void handleListFiles(EthernetClient &client);
    static void handleDownloadFile(EthernetClient &client, const char* filename);
    static void handleDeleteFile(EthernetClient &client, const char* filename);
    static void handleRenameFile(EthernetClient &client, const char* oldName, const char* newName);
    static void handleFileExists(EthernetClient &client, const char* filename);
    static void handleUploadFile(EthernetClient &client, const Request &req);
    
    // Safe Mode handlers
    static void handleSafeModeStatus(EthernetClient &client);
    static void handleSafeModeExit(EthernetClient &client);
    
};

#endif // WEBSERVER_H
