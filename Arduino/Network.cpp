/*
 * Network.cpp
 * Network management implementation
 * Version 3.6
 */

#include "Network.h"

// Static variable initialization
IPAddress NetworkManager::_localIP;
IPAddress NetworkManager::_gateway;
IPAddress NetworkManager::_subnet;
IPAddress NetworkManager::_dns(8, 8, 8, 8);
byte NetworkManager::_mac[6];

bool NetworkManager::begin() {
  StorageManager::generateMacAddress(_mac);
  StorageManager::loadNetworkConfig(_localIP, _gateway, _subnet);
  return restartEthernet();
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
  
  _localIP = newIP;
  StorageManager::saveNetworkConfig(_localIP, _gateway, _subnet);
  
  Serial.println(F("IP saved, restarting network..."));
  return restartEthernet();
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
  return restartEthernet();
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
  return restartEthernet();
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

bool NetworkManager::restartEthernet() {
  Ethernet.maintain();
  Ethernet.begin(_mac, _localIP, _dns, _gateway, _subnet);
  delay(1000);
  return (Ethernet.linkStatus() != LinkOFF);
}
