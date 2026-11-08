/*
 * Network.cpp
 * Network management implementation
 * Version 3.8
 */

#include "Network.h"
#include "VmixConnector.h"
#include "NetUtils.h"
#include <utility/w5100.h>
#include <avr/wdt.h>

// FIN budget per connection when the local IP changes
#define NET_CLOSE_TIMEOUT_MS 50
// Wait for the PHY link after the first (resetting) init
#define NET_LINK_WAIT_MS 1000

// Static variable initialization
IPAddress NetworkManager::_localIP;
IPAddress NetworkManager::_gateway;
IPAddress NetworkManager::_subnet;
IPAddress NetworkManager::_dns(8, 8, 8, 8);
byte NetworkManager::_mac[6];

bool NetworkManager::begin() {
  StorageManager::generateMacAddress(_mac);
  StorageManager::loadNetworkConfig(_localIP, _gateway, _subnet);
  Ethernet.begin(_mac, _localIP, _dns, _gateway, _subnet);
  // Retransmission timing stays at the chip defaults, as in v3.7.1. Do not
  // call Ethernet.setRetransmissionTimeout/Count: Ethernet 2.x writes them at
  // the W5100 addresses, which on the W5500 hit SIR/SIMR and the RTR high
  // byte. A short retry time would also drop vMix and Companion on a brief
  // network glitch.
  
  // The first init resets the chip: give the PHY time to report the link
  unsigned long start = millis();
  while (Ethernet.linkStatus() == LinkOFF && millis() - start < NET_LINK_WAIT_MS) {
    wdt_reset();
    delay(10);
  }
  return (Ethernet.linkStatus() != LinkOFF);
}

void NetworkManager::printConfig() {
  Serial.println(F("Network configuration:"));
  Serial.print(F("IP: "));
  Serial.println(_localIP);
  Serial.print(F("Subnet: "));
  Serial.println(_subnet);
  Serial.print(F("Gateway: "));
  Serial.println(_gateway);
  Serial.print(F("DNS: "));
  Serial.println(_dns);
  Serial.print(F("MAC: "));
  for (int i = 0; i < 6; i++) {
    if (_mac[i] < 16) Serial.print('0');
    Serial.print(_mac[i], HEX);
    if (i < 5) Serial.print(':');
  }
  Serial.println();
}

bool NetworkManager::isValidIP(const char* ip) {
  IPAddress testIP;
  return parseIP(ip, testIP);
}

bool NetworkManager::isValidIP(const String &ip) {
  return isValidIP(ip.c_str());
}

bool NetworkManager::parseIP(const char* ip, IPAddress &outIP) {
  if (!ip) return false;
  
  int parts[4] = {0, 0, 0, 0};
  int partIdx = 0;
  int dotCount = 0;
  
  const char* ptr = ip;
  const char* start = ip;
  
  while (*ptr != '\0' && partIdx < 4) {
    if (*ptr == '.') {
      dotCount++;
      
      int len = ptr - start;
      if (len <= 0 || len > 3) return false;
      
      char numBuf[4];
      strncpy(numBuf, start, len);
      numBuf[len] = '\0';
      
      int val = atoi(numBuf);
      if (val < 0 || val > 255) return false;
      
      parts[partIdx++] = val;
      start = ptr + 1;
    }
    ptr++;
  }
  
  // Last octet
  if (partIdx == 3 && *start != '\0') {
    int len = ptr - start;
    if (len <= 0 || len > 3) return false;
    
    char numBuf[4];
    strncpy(numBuf, start, len);
    numBuf[len] = '\0';
    
    int val = atoi(numBuf);
    if (val < 0 || val > 255) return false;
    
    parts[3] = val;
    partIdx = 4;
  }
  
  if (dotCount != 3 || partIdx != 4) return false;
  
  outIP = IPAddress(parts[0], parts[1], parts[2], parts[3]);
  return true;
}

bool NetworkManager::parseIP(const String &ip, IPAddress &outIP) {
  return parseIP(ip.c_str(), outIP);
}

bool NetworkManager::setLocalIP(const char* ip) {
  IPAddress newIP;
  if (!parseIP(ip, newIP)) return false;
  
  Serial.print(F("Changing IP: "));
  Serial.print(_localIP);
  Serial.print(F(" -> "));
  Serial.println(newIP);
  
  bool changed = !(newIP == _localIP);
  _localIP = newIP;
  StorageManager::saveNetworkConfig(_localIP, _gateway, _subnet);
  
  Serial.println(F("IP saved, restarting network..."));
  return restartEthernet(changed);
}

bool NetworkManager::setLocalIP(const String &ip) {
  return setLocalIP(ip.c_str());
}

bool NetworkManager::setGateway(const char* ip) {
  IPAddress newGateway;
  if (!parseIP(ip, newGateway)) return false;
  
  Serial.print(F("Changing gateway: "));
  Serial.print(_gateway);
  Serial.print(F(" -> "));
  Serial.println(newGateway);
  
  _gateway = newGateway;
  StorageManager::saveNetworkConfig(_localIP, _gateway, _subnet);
  
  Serial.println(F("Gateway saved, restarting network..."));
  return restartEthernet(false);
}

bool NetworkManager::setGateway(const String &ip) {
  return setGateway(ip.c_str());
}

bool NetworkManager::setSubnet(const char* ip) {
  IPAddress newSubnet;
  if (!parseIP(ip, newSubnet)) return false;
  
  Serial.print(F("Changing subnet: "));
  Serial.print(_subnet);
  Serial.print(F(" -> "));
  Serial.println(newSubnet);
  
  _subnet = newSubnet;
  StorageManager::saveNetworkConfig(_localIP, _gateway, _subnet);
  
  Serial.println(F("Subnet saved, restarting network..."));
  return restartEthernet(false);
}

bool NetworkManager::setSubnet(const String &ip) {
  return setSubnet(ip.c_str());
}

IPAddress NetworkManager::getLocalIP() {
  return _localIP;
}

IPAddress NetworkManager::getGateway() {
  return _gateway;
}

IPAddress NetworkManager::getSubnet() {
  return _subnet;
}

IPAddress NetworkManager::getDNS() {
  return _dns;
}

void NetworkManager::getMACAddress(byte *mac) {
  if (mac != NULL) {
    memcpy(mac, _mac, 6);
  }
}

// Closes every TCP connection (vMix, SSE, Companion 8098, HTTP): FIN, a
// short wait, then CLOSE. LISTEN and CLOSED sockets are left alone. The
// modules that held these sockets see them closed (or reused) on their next
// identity check and forget them.
static void closeAllConnections() {
  uint8_t count = (W5100.getChip() == 51) ? 4 : MAX_SOCK_NUM;
  for (uint8_t s = 0; s < count; s++) {
    EthernetClient c(s);
    NetUtils::closeGracefully(c, NET_CLOSE_TIMEOUT_MS);
  }
}

// Hot reconfiguration (BOOT-04). After the first call Ethernet.begin() does
// not reset the chip: it only rewrites MAC, IP, gateway and mask, and every
// socket stays as it was. A connection made from the old IP is dead after
// an IP change, but nothing tells either side (vMix keeps sending to the
// old address, no RST comes back), so all of them are closed first, while
// their FIN still leaves from the old address. LISTEN sockets answer on the
// new IP; the servers re-create any listener they miss on their own. A
// gateway or mask change leaves the open connections working.
bool NetworkManager::restartEthernet(bool ipChanged) {
  if (ipChanged) {
    VmixConnector::closeForNetworkChange();
    closeAllConnections();
  }
  Ethernet.begin(_mac, _localIP, _dns, _gateway, _subnet);
  if (ipChanged) {
    VmixConnector::reconnectNow();
  }
  return (Ethernet.linkStatus() != LinkOFF);
}
