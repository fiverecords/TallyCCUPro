/*
 * SdUtils.h
 * SD card utilities and preset management
 * Version 3.8
 */

#ifndef SD_UTILS_H
#define SD_UTILS_H

#include "Configuration.h"
#include "CCUControl.h"

// Buffer sizes for SD - reduced for memory optimization
#define SD_LINE_BUFFER_SIZE 128
#define SD_PATH_BUFFER_SIZE 32

// Card recovery (SD-03)
#define SD_REMOUNT_MS 10000UL       // min time between begin() attempts

// serveFile() send budget (WDT-01/WDT-02). The vMix tally, the PTZ deadman
// and the port-80 listener keep running while a file is sent
// (WebServer::transferPoll()), but other HTTP requests and 8098 wait: the
// cap bounds that stall. A slower client gets a cut it can retry
// (index.html.gz: ~5 s budget).
#define SD_SEND_BLOCK_MS 2000UL     // one block: wait for room at most this,
#define SD_SEND_SLICE_MS 20UL       // calling transferPoll() every slice
#define SD_SEND_BASE_MS 4000UL      // whole file: base...
#define SD_SEND_BYTES_PER_MS 32     // ...plus its size at 32 KB/s...
#define SD_SEND_MAX_MS 10000UL      // ...capped
#define SD_CUT_FIN_MS 300UL         // cut transfer: wait this for a FIN behind the data
#define SD_LIST_MS 3000UL           // preset list: total budget (Companion waits 3 s)...
#define SD_LIST_WALK_MS 2500UL      // ...of which the directory walk at most

class SdUtils {
  public:
    // Mounts the card (boot and remounts)
    static bool begin();

    // True while the card is mounted and no card error has been seen since
    // (direct getSd() users included). Otherwise remounts it, at most every
    // SD_REMOUNT_MS (begin() takes ~2 s without a card), and returns the
    // result; in between it fails at once. Call before any getSd() use.
    static bool isAvailable();

    // SD instance access
    static SdFat& getSd();

    // Basic file operations (false at once while the SD is not available).
    // After a card error (card reinserted) they remount and retry once.
    static bool fileExists(const char* filename);
    static bool removeFile(const char* filename);
    static bool renameFile(const char* oldName, const char* newName);

    // Deletes '<filename>.gz', which serveFile() prefers over <filename>.
    // Call after writing, deleting or renaming <filename> (upload, SD
    // manager) so a stale copy is not served. No-op for a .gz name. Uses
    // the internal line buffer.
    static void invalidateGzip(const char* filename);

    // Preset management
    static bool applyPreset(int cameraId, int presetId);

    // Body of /?listPresets: {"presets":[...]}, plus "error":"sd" when the
    // card is not available or fails and "truncated":true when the walk ran
    // out of time. NetUtils block writes within SD_LIST_MS, transferPoll()
    // while the reader is slow (no txIdle wait needed before it);
    // false if the client could not take it (it is then closed at once).
    static bool writePresetListToClient(EthernetClient &client);

    // Writes the whole response: 200 with Content-Length, 404, or 503 when
    // the SD is not available or fails. acceptsGzip: the request had
    // Accept-Encoding: gzip; <file>.gz is then sent if present, with
    // Content-Encoding: gzip and the original's Content-Type. Bounded sends
    // (watchdog, tally and deadman kept alive). True only if the whole file
    // was sent; a cut transfer closes the client (FIN if the reader is still
    // reading, otherwise at once). On false do not write another response
    // (a 404/503 has been sent or the client is closed): return after
    // serveFile() whatever its result.
    static bool serveFile(EthernetClient &client, const char* filename, bool acceptsGzip = false);

    // URL decode
    static void urlDecodeInPlace(char* str);

    // Concurrency control for SD
    static bool tryLockSD(unsigned long timeout = 1000);
    static void unlockSD();

    // Generate preset filename
    static void getPresetFilename(char* buffer, int bufSize, int cameraId, int presetId, bool isTemp = false);

  private:
    static SdFat _sd;

    // Buffers for optimized operations
    static uint8_t _sdBuffer[SD_BUFFER_SIZE];
    static char _lineBuffer[SD_LINE_BUFFER_SIZE];
};

#endif // SD_UTILS_H
