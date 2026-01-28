/*
 * Network.h
 * Network management and Ethernet configuration
 * Version 3.6
 */

#ifndef NETWORK_H
#define NETWORK_H

#include "Configuration.h"
#include "Storage.h"

class NetworkManager {
  public:
    static bool begin();
    static void printConfig();
    
    // Validate IP address
    static bool isValidIP(const char* ip);
    static bool isValidIP(const String &ip);
    
    // Configuration setters
    static bool setLocalIP(const char* ip);
    static bool setGateway(const char* ip);
    static bool setSubnet(const char* ip);
    
    // String wrappers for compatibility
    static bool setLocalIP(const String &ip);
    static bool setGateway(const String &ip);
    static bool setSubnet(const String &ip);
    
    // Parse IP string to IPAddress
    static bool parseIP(const char* ip, IPAddress &outIP);
    static bool parseIP(const String &ip, IPAddress &outIP);
    
    // Getters
    static IPAddress getLocalIP();
    static IPAddress getGateway();
    static IPAddress getSubnet();
    static IPAddress getDNS();
    static void getMACAddress(byte *mac);

  private:
    static IPAddress _localIP;
    static IPAddress _gateway;
    static IPAddress _subnet;
    static IPAddress _dns;
    static byte _mac[6];
    
    static bool restartEthernet();
};

#endif // NETWORK_H
