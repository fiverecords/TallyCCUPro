/*
 * TallyManager.h
 * Tally map and camera control management
 * Version 3.0
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
    
    // Set mapping between vMix input and camera
    static void setMapping(byte inputIndex, byte inputNumber, byte cameraId);
    
    // Override management
    static void setOverride(bool enabled);
    static bool getOverride();
    
    // Set tally state for a camera
    static void setTallyState(byte camera, char state);
    
    // Set all states at once (direct application)
    static void setTallyStates(bool states[MAX_CAMERAS + 1][2]);
    
    // Print current tally map
    static void printCurrentMap();

  private:
    // Map: vMix input => Blackmagic camID
    static byte _inputs[MAXTALLIES];
    static byte _cams[MAXTALLIES];
    
    // Tally override
    static bool _overrideTally;
};

#endif // TALLY_MANAGER_H
