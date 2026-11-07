/*
 * VmixConnector.h
 * vMix connection management for Tally
 * Version 3.8
 */

#ifndef VMIX_CONNECTOR_H
#define VMIX_CONNECTOR_H

#include "Configuration.h"
#include "Storage.h"
#include "TallyManager.h"

// connect()/stop() timeout towards vMix. The library default (1000 ms) froze
// the whole loop on every failed attempt; on a LAN the SYN-ACK takes < 10 ms.
#define VMIX_CONNECT_TIMEOUT_MS 250

class VmixConnector {
  public:
    static bool begin();
    static bool begin(const byte vmixip[4]);
    
    static bool connect();
    
    // Process received data (optimized - no String)
    static void processData();
    
    // Read and apply pending vMix tally data without reconnecting. Cheap and
    // safe to call from long-running loops (file transfers).
    static void pollTally();
    
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
    
    // Socket handling: only ever stop() a socket that is still ours
    static bool openSocket();
    static bool ownsSocket();
    static void closeSocket();
    static void dropSocket();
    
    // Apply tally states directly (no String)
    static void applyTallyDirectly(const char* tallyData);
};

#endif // VMIX_CONNECTOR_H
