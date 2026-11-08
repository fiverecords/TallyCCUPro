/*
 * Network.cpp
 * Network management implementation
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
byte NetworkManager::_mac[6];
bool NetworkManager::_hardware = false;

static uint32_t toU32(const byte ip[4]) {
  return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
         ((uint32_t)ip[2] << 8) | ip[3];
}

static uint32_t toU32(const IPAddress &ip) {
  const byte b[4] = {ip[0], ip[1], ip[2], ip[3]};
  return toU32(b);
}

// Apply our settings to the chip. No DNS lookups are made: the gateway
// fills the DNS slot.
static void applySettings(byte mac[6], const IPAddress &ip, const IPAddress &gw,
                          const IPAddress &mask) {
  Ethernet.begin(mac, ip, gw, gw, mask);
}

NetResult NetworkManager::begin() {
  StorageManager::generateMacAddress(_mac);
  StorageManager::loadNetworkConfig(_localIP, _gateway, _subnet);
  applySettings(_mac, _localIP, _gateway, _subnet);
  // Retransmission timing stays at the chip defaults, as in v3.7.1. Do not
  // call Ethernet.setRetransmissionTimeout/Count: Ethernet 2.x writes them at
  // the W5100 addresses, which on the W5500 hit SIR/SIMR and the RTR high
  // byte. A short retry time would also drop vMix and Companion on a brief
  // network glitch.

  // No chip: Ethernet.begin() returned without writing anything, and every
  // later library call would retry the (blocking) init (BOOT-12)
  _hardware = (Ethernet.hardwareStatus() != EthernetNoHardware);
  if (!_hardware) return NET_NO_HARDWARE;

  // The first init resets the chip: give the PHY time to report the link
  unsigned long start = millis();
  while (Ethernet.linkStatus() == LinkOFF && millis() - start < NET_LINK_WAIT_MS) {
    wdt_reset();
    delay(10);
  }
  return linkState();
}

NetResult NetworkManager::linkState() {
  if (!_hardware) return NET_NO_HARDWARE;
  switch (Ethernet.linkStatus()) {
    case LinkON:  return NET_LINK_UP;
    case LinkOFF: return NET_LINK_DOWN;
    default:      return NET_LINK_UNKNOWN;   // W5100: no link status
  }
}

void NetworkManager::printConfig() {
  Serial.println(F("Network configuration:"));
  Serial.print(F("IP: "));
  Serial.println(_localIP);
  Serial.print(F("Subnet: "));
  Serial.println(_subnet);
  Serial.print(F("Gateway: "));
  Serial.println(_gateway);
  Serial.print(F("MAC: "));
  for (int i = 0; i < 6; i++) {
    if (_mac[i] < 16) Serial.print('0');
    Serial.print(_mac[i], HEX);
    if (i < 5) Serial.print(':');
  }
  Serial.println();
}

// A W5500 that resets on its own (ESD, 3.3 V dip) comes back with no IP and
// no sockets. Two reads in a row that disagree with our IP re-apply the
// settings; the servers re-create their listeners and vMix reconnects.
void NetworkManager::maintain() {
  static unsigned long lastCheck = 0;
  static uint8_t mismatches = 0;

  if (!_hardware) return;
  unsigned long now = millis();
  if (now - lastCheck < NET_CHECK_INTERVAL_MS) return;
  lastCheck = now;

  IPAddress chipIP = Ethernet.localIP();
  if (chipIP == _localIP) {
    if (mismatches >= 2) Serial.println(F("Ethernet chip settings OK again"));
    mismatches = 0;
    return;
  }
  if (mismatches < 255) mismatches++;
  if (mismatches < 2) return;   // one odd read: look again next time
  if (mismatches == 2) {
    Serial.print(F("Ethernet chip lost its settings (reads "));
    Serial.print(chipIP);
    Serial.println(F("), re-applying"));
  }
  applySettings(_mac, _localIP, _gateway, _subnet);
}

bool NetworkManager::parseIP(const char* ip, byte out[4]) {
  if (!ip) return false;
  for (byte i = 0; i < 4; i++) {
    uint16_t v = 0;
    byte digits = 0;
    while (*ip >= '0' && *ip <= '9') {
      if (++digits > 3) return false;
      v = v * 10 + (*ip++ - '0');
    }
    if (digits == 0 || v > 255) return false;
    out[i] = (byte)v;
    if (i < 3) {
      if (*ip != '.') return false;
      ip++;
    }
  }
  return *ip == '\0';
}

bool NetworkManager::parseIP(const char* ip, IPAddress &outIP) {
  byte b[4];
  if (!parseIP(ip, b)) return false;
  outIP = IPAddress(b);
  return true;
}

bool NetworkManager::isHostIP(const byte ip[4]) {
  return ip[0] >= 1 && ip[0] <= 223 && ip[0] != 127;
}

bool NetworkManager::isSubnetMask(const byte mask[4]) {
  uint32_t m = toU32(mask);
  uint32_t host = ~m;
  if (m == 0 || (host & (host + 1)) != 0) return false;   // empty or not contiguous
  return host >= 3;   // at most /30
}

// Host part all zeros or all ones under mask
static bool netOrBroadcast(uint32_t ip, uint32_t mask) {
  uint32_t host = ip & ~mask;
  return host == 0 || host == ~mask;
}

bool NetworkManager::inLocalSubnet(const byte ip[4]) {
  uint32_t mask = toU32(_subnet);
  return (toU32(ip) & mask) == (toU32(_localIP) & mask);
}

bool NetworkManager::isLocalNetOrBroadcast(const byte ip[4]) {
  return inLocalSubnet(ip) && netOrBroadcast(toU32(ip), toU32(_subnet));
}

static void warnGateway(const IPAddress &local, const IPAddress &gw, const IPAddress &mask) {
  uint32_t g = toU32(gw);
  if (g == 0) return;   // no gateway
  uint32_t m = toU32(mask);
  if ((g & m) != (toU32(local) & m)) {
    Serial.print(F("Warning: gateway "));
    Serial.print(gw);
    Serial.println(F(" is outside the local subnet"));
  }
}

NetResult NetworkManager::setLocalIP(const char* ip) {
  byte b[4];
  if (!parseIP(ip, b)) {
    Serial.println(F("Error: Invalid IP (use a.b.c.d)"));
    return NET_INVALID;
  }
  if (!isHostIP(b) || netOrBroadcast(toU32(b), toU32(_subnet))) {
    Serial.println(F("Error: not a host address (1-223.x.x.x, not 127.x, not the network or broadcast address)"));
    return NET_INVALID;
  }
  IPAddress newIP(b);

  Serial.print(F("Changing IP: "));
  Serial.print(_localIP);
  Serial.print(F(" -> "));
  Serial.println(newIP);

  bool changed = !(newIP == _localIP);
  _localIP = newIP;
  StorageManager::saveNetworkConfig(_localIP, _gateway, _subnet);
  warnGateway(_localIP, _gateway, _subnet);

  Serial.println(F("IP saved, restarting network..."));
  return restartEthernet(changed);
}

NetResult NetworkManager::setGateway(const char* ip) {
  byte b[4];
  if (!parseIP(ip, b)) {
    Serial.println(F("Error: Invalid gateway (use a.b.c.d)"));
    return NET_INVALID;
  }
  IPAddress newGateway(b);
  if (toU32(b) != 0 && (!isHostIP(b) || newGateway == _localIP || isLocalNetOrBroadcast(b))) {
    Serial.println(F("Error: not a usable gateway (0.0.0.0 for none)"));
    return NET_INVALID;
  }

  Serial.print(F("Changing gateway: "));
  Serial.print(_gateway);
  Serial.print(F(" -> "));
  Serial.println(newGateway);

  _gateway = newGateway;
  StorageManager::saveNetworkConfig(_localIP, _gateway, _subnet);
  warnGateway(_localIP, _gateway, _subnet);

  Serial.println(F("Gateway saved, restarting network..."));
  return restartEthernet(false);
}

NetResult NetworkManager::setSubnet(const char* ip) {
  byte b[4];
  if (!parseIP(ip, b) || !isSubnetMask(b)) {
    Serial.println(F("Error: Invalid subnet (contiguous mask, /1 to /30)"));
    return NET_INVALID;
  }
  if (netOrBroadcast(toU32(_localIP), toU32(b))) {
    Serial.println(F("Error: the local IP would be the network or broadcast address"));
    return NET_INVALID;
  }
  IPAddress newSubnet(b);

  Serial.print(F("Changing subnet: "));
  Serial.print(_subnet);
  Serial.print(F(" -> "));
  Serial.println(newSubnet);

  _subnet = newSubnet;
  StorageManager::saveNetworkConfig(_localIP, _gateway, _subnet);
  warnGateway(_localIP, _gateway, _subnet);

  Serial.println(F("Subnet saved, restarting network..."));
  return restartEthernet(false);
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
// gateway or mask change leaves the open connections working. The new
// values are saved before: without a chip they apply at the next boot.
NetResult NetworkManager::restartEthernet(bool ipChanged) {
  if (!_hardware) return NET_NO_HARDWARE;   // never block on a missing chip
  if (ipChanged) {
    VmixConnector::closeForNetworkChange();
    closeAllConnections();
  }
  applySettings(_mac, _localIP, _gateway, _subnet);
  if (ipChanged) {
    VmixConnector::reconnectNow();
  }
  return linkState();
}
