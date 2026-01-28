/*
 * VmixConnector.h
 * vMix connection management for Tally
 * Version 3.6
 */

#ifndef VMIX_CONNECTOR_H
#define VMIX_CONNECTOR_H

#include "Configuration.h"
#include "Storage.h"
#include "TallyManager.h"

class VmixConnector {
  public:
    static bool begin();
    static bool begin(const byte vmixip[4]);
    
    static bool connect();
    
    // Process received data (optimized - no String)
    static void processData();
    
    // Change IP and save to EEPROM
    static bool setVmixIP(const byte vmixip[4]);
    
    // Get current vMix IP
    static void getVmixIP(byte vmixip[4]);
    
    // Enable/disable connection attempt
    static void setConnectEnabled(bool enabled);
    static bool getConnectEnabled();
    
    // Get actual TCP connection status
    static bool isConnected();

  private:
    static EthernetClient _vmixClient;
    static byte _vmixip[4];
    static bool _connectEnabled;
    
    // Apply tally states directly (no String)
    static void applyTallyDirectly(const char* tallyData);
};

#endif // VMIX_CONNECTOR_H
