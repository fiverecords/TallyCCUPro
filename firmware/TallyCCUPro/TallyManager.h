/*
 * TallyManager.h
 * Tally map and camera control management
 */

#ifndef TALLY_MANAGER_H
#define TALLY_MANAGER_H

#include "Configuration.h"
#include "Storage.h"

class TallyManager {
  public:
    static bool begin();

    // Load/save tally map from/to EEPROM
    static void loadMap();
    static void saveMap();

    // Set mapping between vMix input and camera. 0 keeps that half; a slot
    // left with only one valid half is emptied
    static void setMapping(byte inputIndex, byte inputNumber, byte cameraId);

    // Get mapping at index (0/0 for an empty or invalid slot)
    static void getMapping(byte index, byte &inputNumber, byte &cameraId);

    // Empty a mapping slot (input 0, camera 0)
    static void clearMapping(byte index);

    // Override management
    static void setOverride(bool enabled);
    static bool getOverride();

    // Set all states at once. Only cameras that changed, or whose last
    // write failed, are written to the shield.
    static void setTallyStates(bool states[MAX_CAMERAS + 1][2]);

    // Turn every camera tally off (e.g. when vMix is lost or disabled)
    static void clearAll();

    // Last vMix state of the mapped cameras, bit 0 = camera 1, also with
    // the override off or in Safe Mode: what is on air (UI-18)
    static void getAir(uint8_t &pgm, uint8_t &pvw);

    // Called from loop(): re-sends one camera every TALLY_REFRESH_MS (TAL-12)
    static void refresh();

    // Print current tally map
    static void printCurrentMap();

    // True once after the map or the override changed (TAL-09): the vMix
    // connector then applies its last state again
    static bool takeReapply();

  private:
    // Map: vMix input => Blackmagic camID
    static byte _inputs[MAXTALLIES];
    static byte _cams[MAXTALLIES];

    // Tally override
    static bool _overrideTally;

    static bool _reapply;

    // Slot with an input in 1..MAX_VMIX_INPUTS and a camera in 1..MAX_CAMERAS
    static bool validSlot(byte index);
};

#endif // TALLY_MANAGER_H
