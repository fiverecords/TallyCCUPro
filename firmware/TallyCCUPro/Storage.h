/*
 * Storage.h
 * EEPROM storage management and configuration persistence
 */

#ifndef STORAGE_H
#define STORAGE_H

#include "Configuration.h"

class StorageManager {
  public:
    // Check the layout header (BOOT-16): EEPROM without one (blank, or the
    // same map written by v3.7.1/v3.8) keeps every stored value; only cells
    // never written get their default. Call before any other load.
    static void begin();

    // Factory settings: network, vMix IP, tally map, overrides, Safe Mode
    // flags. The MAC is kept. Takes effect after a restart.
    static void factoryReset();

    // 16-bit integer read/write
    static void writeInt(int address, int number);
    static int readInt(int address);

    // Byte read/write
    static void writeByte(int address, byte value);
    static byte readByte(int address);

    // Network configuration
    static void loadNetworkConfig(IPAddress &localIP, IPAddress &gateway, IPAddress &subnet);
    static void saveNetworkConfig(const IPAddress &localIP, const IPAddress &gateway, const IPAddress &subnet);

    // vMix IP (factory default when never written)
    static void loadVmixIP(byte ip[4]);
    static void saveVmixIP(const byte ip[4]);

    // Tally maps
    static void loadTallyMap(byte inputs[MAXTALLIES], byte cams[MAXTALLIES]);
    static void saveTallyMap(const byte inputs[MAXTALLIES], const byte cams[MAXTALLIES]);

    // Overrides (2 params)
    static void loadOverrides(bool &tallyOverride, bool &ccuOverride);
    static void saveOverrides(bool tallyOverride, bool ccuOverride);

    // Overrides with vMix connect state (3 params)
    static void loadOverrides(bool &tallyOverride, bool &ccuOverride, bool &vmixConnect);
    static void saveOverrides(bool tallyOverride, bool ccuOverride, bool vmixConnect);

    // MAC address: the stored one (full or legacy), generated on first boot
    static void generateMacAddress(byte mac[6]);
    // Store a full MAC (serial "mac"); false if not a unicast address
    static bool saveMacAddress(const byte mac[6]);

  private:
    // Write byte only if changed (saves EEPROM write cycles)
    static void writeByteIfChanged(int address, byte value);
    static void writeNetworkDefaults();
    static void writeVmixDefault();
};

#endif // STORAGE_H
