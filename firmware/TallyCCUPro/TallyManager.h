/*
 * TallyManager.h
 * Tally map and camera control management
 * Version 3.8
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
    
    // Map vMix input to Blackmagic camera ID
    static byte mapInputToCamera(byte input);
    
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
    
    // Set tally state for a camera
    static void setTallyState(byte camera, char state);
    
    // Set all states at once (direct application)
    static void setTallyStates(bool states[MAX_CAMERAS + 1][2]);
    
    // Turn every camera tally off (e.g. when vMix is lost or disabled)
    static void clearAll();
    
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
