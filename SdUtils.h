/*
 * SdUtils.h
 * SD card utilities and preset management
 * Version 3.0
 */

#ifndef SD_UTILS_H
#define SD_UTILS_H

#include "Configuration.h"
#include "CCUControl.h"

// Buffer sizes for SD - reduced for memory optimization
#define SD_LINE_BUFFER_SIZE 128
#define SD_PATH_BUFFER_SIZE 32

class SdUtils {
  public:
    static bool begin();
    
    // SD instance access
    static SdFat& getSd();
    
    // Basic file operations
    static bool fileExists(const char* filename);
    static bool removeFile(const char* filename);
    static bool renameFile(const char* oldName, const char* newName);
    static void optimizedWriteFile(File &file, const uint8_t *buffer, size_t size);
    static void optimizedReadFile(File &file, uint8_t *buffer, size_t size);
    
    // Preset management
    static bool savePreset(int cameraId, int presetId, const char* presetName, const char* presetData);
    static bool applyPreset(int cameraId, int presetId);
    
    // Write preset list directly to client (no intermediate String)
    static void writePresetListToClient(EthernetClient &client);
    
    // Fragmented preset handling
    static bool initPresetFragmented(int cameraId, int presetId, const char* presetName);
    static bool addPresetFragment(int cameraId, int presetId, int fragmentNumber, const char* fragmentData);
    static bool finalizePresetFragmented(int cameraId, int presetId, int totalFragments);
    
    // Serve files for web
    static bool serveFile(EthernetClient &client, const char* filename);
    
    // URL decode functions
    static void urlDecodeInPlace(char* str);
    static String urlDecode(const String &input);

    // Concurrency control for SD
    static bool tryLockSD(unsigned long timeout = 1000);
    static void unlockSD();
    static bool isApplyingPreset();
    
    // Generate preset filename
    static void getPresetFilename(char* buffer, int bufSize, int cameraId, int presetId, bool isTemp = false);
    
  private:
    static SdFat _sd;
    
    // Fragmented preset variables
    static File _presetFragmentedFile;
    static bool _presetFragmentedInProgress;
    static int _currentFragmentedPresetCamera;
    static int _currentFragmentedPresetId;
    static int _receivedFragmentsCount;
    
    // Buffers for optimized operations
    static uint8_t _sdBuffer[SD_BUFFER_SIZE];
    static char _lineBuffer[SD_LINE_BUFFER_SIZE];
};

#endif // SD_UTILS_H
