/*
 * SdUtils.h
 * SD card utilities and preset management
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

// Blocking sends (preset list, error pages): the vMix tally, the PTZ
// deadman and the port-80 listener keep running (WebServer::transferPoll())
#define SD_SEND_BLOCK_MS 2000UL     // one block: wait for room at most this,
#define SD_SEND_SLICE_MS 20UL       // calling transferPoll() every slice
#define SD_LIST_MS 3000UL           // preset list: total budget (Companion waits 3 s)...
#define SD_LIST_WALK_MS 2500UL      // ...of which the directory walk at most

// Background file transfers (serveFile): pumped a little on every
// WebServer::processRequests() pass, so other requests (a PTZ stop
// included) are answered meanwhile. Each one holds a socket and 82 bytes.
#define SD_XFERS 2                  // transfers at once; further page/file
                                    // requests wait in their socket
#define SD_XFER_PASS_MS 6UL         // pump time per pass, all transfers
#define SD_SEND_BASE_MS 5000UL      // body budget: base...
#define SD_SEND_BYTES_PER_MS 8      // ...plus its size at 8 KB/s...
#define SD_SEND_MAX_MS 30000UL      // ...capped: then cut
#define SD_SEND_STALL_MS 5000UL     // reader took nothing this long: cut
#define SD_SEND_FIN_MS 3000UL       // after the body or a cut: FIN once the data
                                    // is out, else closed at once (no FIN)
#define SD_SEND_LINGER_MS 5000UL    // after the FIN: wait for the close, then force it
                                    // (a slow reader still takes the last data)
#define SD_LINGERS 3                // sockets waiting so, outside the slots (9 bytes
                                    // each); one more closes the oldest at once

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
    // Removing a file (removeFile, replaceFile's target) first cuts any
    // transfer reading it: its clusters are freed and may be reused.
    static bool fileExists(const char* filename);
    static bool removeFile(const char* filename);
    static bool renameFile(const char* oldName, const char* newName);

    // Commits a fully written temporary file: replaces dst (if any) with
    // tmp (SdFat rename() fails if the target exists). If dst cannot be
    // removed, tmp is removed and dst kept; if the rename fails after that,
    // tmp is kept (the only copy). The single last step of every atomic
    // save (presets, uploads): callers do not remove tmp after it.
    static bool replaceFile(const char* tmp, const char* dst);

    // One line without its terminator ('\n', '\r' or "\r\n"): length, -1 at
    // end of file, -2 on a read error. A line that does not fit is
    // consumed whole and flagged in *cut.
    static int readLine(File& f, char* buf, size_t size, bool* cut);

    // Deletes '<filename>.gz', which serveFile() prefers over <filename>.
    // Call after writing, deleting or renaming <filename> (upload, SD
    // manager) so a stale copy is not served. No-op for a .gz name. Uses
    // the internal line buffer.
    static void invalidateGzip(const char* filename);

    // Preset management. Files hold decoded values and names (as saved
    // by every version): they are never URL-decoded again (SD-05).
    static bool applyPreset(int cameraId, int presetId);

    // Body of /?listPresets: {"presets":[...]}, plus "error":"sd" when the
    // card is not available or fails and "truncated":true when the walk ran
    // out of time. NetUtils block writes within SD_LIST_MS, transferPoll()
    // while the reader is slow (no txIdle wait needed before it);
    // false if the client could not take it (it is then closed at once).
    static bool writePresetListToClient(EthernetClient &client);

    // Answers a GET with a file: 200 with Content-Length and the headers,
    // then the body as a background transfer (pumpTransfers()). The
    // transfer takes over the socket and releases 'client' (the caller's
    // close is then a no-op). acceptsGzip: <file>.gz is sent if present,
    // with Content-Encoding: gzip and the original's Content-Type.
    // attachment: a download (octet-stream, Content-Disposition with the
    // name, which must not contain '"' or control characters; no .gz).
    // Otherwise a 404, a 503 when the SD is not available or fails, or a
    // 503 busy when no transfer slot is free; the caller then closes.
    // A body cut (budget, stalled reader, read error) ends with a FIN when
    // the reader is still reading (Content-Length tells it), else at once.
    // In every case do not write another response, except with unreadable:
    // if not NULL, a file that cannot be read (missing, SD not available or
    // failing before anything was sent) is not answered and *unreadable is
    // set, so the caller answers instead (built-in Safe Mode page, BOOT-13).
    // True if the transfer was started.
    static bool serveFile(EthernetClient &client, const char* filename, bool acceptsGzip,
                          bool attachment = false, bool* unreadable = NULL);

    // Moves every transfer on (at most SD_XFER_PASS_MS). Uses _sdBuffer:
    // only from the top of WebServer::processRequests(), never inside a
    // request (transferPoll() does not call it).
    static void pumpTransfers();

    // True if a transfer holds socket index sock (the server skips it)
    static bool transferOwns(uint8_t sock);

    // True if serveFile() can start a transfer now
    static bool transferSlotFree();

    // Generate preset filename
    static void getPresetFilename(char* buffer, int bufSize, int cameraId, int presetId, bool isTemp = false);

  private:
    static SdFat _sd;

    // Buffers for optimized operations
    static uint8_t _sdBuffer[SD_BUFFER_SIZE];
    static char _lineBuffer[SD_LINE_BUFFER_SIZE];
};

#endif // SD_UTILS_H
