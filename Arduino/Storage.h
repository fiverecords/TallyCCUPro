/*
 * Storage.h
 * EEPROM storage management and configuration persistence
 * Version 3.6
 */

#ifndef STORAGE_H
#define STORAGE_H

#include "Configuration.h"

class StorageManager {
  public:
    static void begin();
    
    // 16-bit integer read/write
    static void writeInt(int address, int number);
    static int readInt(int address);
    
    // Byte read/write
    static void writeByte(int address, byte value);
    static byte readByte(int address);
    
    // Network configuration
    static void loadNetworkConfig(IPAddress &localIP, IPAddress &gateway, IPAddress &subnet);
    static void saveNetworkConfig(const IPAddress &localIP, const IPAddress &gateway, const IPAddress &subnet);
    
    // Tally maps
    static void loadTallyMap(byte inputs[MAXTALLIES], byte cams[MAXTALLIES]);
    static void saveTallyMap(const byte inputs[MAXTALLIES], const byte cams[MAXTALLIES]);
    
    // Overrides (2 params)
    static void loadOverrides(bool &tallyOverride, bool &ccuOverride);
    static void saveOverrides(bool tallyOverride, bool ccuOverride);
    
    // Overrides with vMix connect state (3 params)
    static void loadOverrides(bool &tallyOverride, bool &ccuOverride, bool &vmixConnect);
    static void saveOverrides(bool tallyOverride, bool ccuOverride, bool vmixConnect);
    
    // MAC address generation
    static void generateMacAddress(byte mac[6]);

  private:
    // Write byte only if changed (wear leveling)
    static void writeByteIfChanged(int address, byte value);
};

#endif // STORAGE_H
