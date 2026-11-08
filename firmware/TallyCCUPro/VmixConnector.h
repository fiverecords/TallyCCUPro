/*
 * VmixConnector.h
 * vMix connection management for Tally
 */

#ifndef VMIX_CONNECTOR_H
#define VMIX_CONNECTOR_H

#include "Configuration.h"
#include "Storage.h"
#include "TallyManager.h"

// connect() timeout towards vMix. The library default (1000 ms) froze the
// whole loop on every failed attempt; on a LAN the SYN-ACK takes < 10 ms.
// connect() cannot be split into start + poll from outside the library
// (EthernetClass::socketBegin/socketConnect are private and the RX state
// they reset lives in socket.cpp), so an unanswered attempt still blocks
// the loop this long. It runs from processData() only, never from an HTTP
// handler or pollTally(). A refused connect (RST) returns at once.
#define VMIX_CONNECT_TIMEOUT_MS 250
// Budget for an intentional close (FIN, wait for vMix to answer it)
#define VMIX_CLOSE_TIMEOUT_MS 100

struct TallyFrame;

class VmixConnector {
  public:
    // Load the connection state; the first attempt runs from processData()
    static bool begin(const byte vmixip[4]);

    // Ask for a connection attempt on the next processData() pass
    static bool connect();

    // Main loop: read and apply tally, heartbeat, reconnection
    static void processData();

    // Read and apply pending vMix tally data and detect a lost link (tally
    // off) without reconnecting. Cheap and safe to call from long-running
    // loops (file transfers); a no-op while processData() runs.
    static void pollTally();

    // Validate, save to EEPROM and use the new IP (connects on the next
    // processData()). False, and nothing changed, if it is not a usable
    // host address (BOOT-11). The only place that writes the vMix IP.
    static bool setVmixIP(const byte vmixip[4]);

    // Get current vMix IP
    static void getVmixIP(byte vmixip[4]);

    // Enable/disable connection attempt
    static void setConnectEnabled(bool enabled);
    static bool getConnectEnabled();

    // Connected and vMix answered recently
    static bool isConnected();

    // Local IP change: close the connection while the old address still
    // reaches vMix (call BEFORE reconfiguring the chip), then reconnect
    static void closeForNetworkChange();
    static void reconnectNow();

  private:
    static EthernetClient _vmixClient;
    static byte _vmixip[4];
    static bool _connectEnabled;

    // Socket handling: only ever close a socket that is still ours
    static int8_t openSocket();
    static bool ownsSocket();
    static void closeSocket(bool graceful);
    static void dropSocket();
    static void connectionLost(bool graceful);
    static void tryReconnect(unsigned long now);

    // Shared by processData() and pollTally()
    static bool serviceLink(unsigned long now, bool mayWait);
    static void readInput(unsigned long now);
    static void heartbeat(unsigned long now);
    static void onTallyLine(const TallyFrame& data, unsigned long now);
    static void applyPending(unsigned long now);
    static void reapply();

    // Apply tally states directly (no String)
    static void applyTallyDirectly(const TallyFrame& tallyData);
};

#endif // VMIX_CONNECTOR_H
