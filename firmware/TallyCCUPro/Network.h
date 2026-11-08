/*
 * Network.h
 * Network management and Ethernet configuration
 */

#ifndef NETWORK_H
#define NETWORK_H

#include "Configuration.h"
#include "Storage.h"

// Outcome of begin() and of the setters (BOOT-10, BOOT-12)
enum NetResult : uint8_t {
  NET_INVALID = 0,      // rejected, nothing saved
  NET_LINK_UP = 1,      // applied, Ethernet link up
  NET_LINK_DOWN = 2,    // applied (saved), no Ethernet link: cable unplugged?
  NET_LINK_UNKNOWN = 3, // applied, chip without link status (W5100)
  NET_NO_HARDWARE = 4   // saved, the Ethernet chip does not answer
};

class NetworkManager {
  public:
    static NetResult begin();
    static void printConfig();

    // Every loop() pass: every NET_CHECK_INTERVAL_MS checks that the chip
    // still holds our settings and re-applies them if it lost them (a reset
    // of the W5500 by ESD or a supply dip)
    static void maintain();

    // Strict "a.b.c.d" (BOOT-11): four decimal octets 0..255, 1-3 digits
    // each, nothing before or after. Syntax only.
    static bool parseIP(const char* ip, byte out[4]);
    static bool parseIP(const char* ip, IPAddress &outIP);
    // Address a host can use: 1-223, not 127 (loopback), so not 0.x,
    // multicast or 255.x
    static bool isHostIP(const byte ip[4]);
    // Contiguous mask, /1 to /30
    static bool isSubnetMask(const byte mask[4]);
    // Network or broadcast address of the local subnet
    static bool isLocalNetOrBroadcast(const byte ip[4]);
    static bool inLocalSubnet(const byte ip[4]);

    // Configuration setters: validate, save to EEPROM, apply. A rejected
    // value is explained on Serial and returns NET_INVALID.
    static NetResult setLocalIP(const char* ip);
    static NetResult setGateway(const char* ip);
    static NetResult setSubnet(const char* ip);

    // Getters
    static IPAddress getLocalIP();
    static IPAddress getGateway();
    static IPAddress getSubnet();
    static void getMACAddress(byte *mac);
    static NetResult linkState();

  private:
    static IPAddress _localIP;
    static IPAddress _gateway;
    static IPAddress _subnet;
    static byte _mac[6];
    static bool _hardware;

    // Reconfigure the running chip; ipChanged closes every connection
    static NetResult restartEthernet(bool ipChanged);
};

#endif // NETWORK_H
