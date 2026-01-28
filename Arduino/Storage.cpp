/*
 * Storage.cpp
 * EEPROM storage implementation
 * Version 3.6
 * 
 * Features EEPROM wear leveling (only writes if value changed)
 */

#include "Storage.h"

void StorageManager::begin() {
  // No specific initialization required for Arduino EEPROM
}

// Write byte only if changed (wear leveling)
void StorageManager::writeByteIfChanged(int address, byte value) {
  if (EEPROM.read(address) != value) {
    EEPROM.write(address, value);
  }
}

void StorageManager::writeInt(int address, int number) {
  writeByteIfChanged(address, number >> 8);       // High byte
  writeByteIfChanged(address + 1, number & 0xFF); // Low byte
}

int StorageManager::readInt(int address) {
  return (EEPROM.read(address) << 8) + EEPROM.read(address + 1);
}

void StorageManager::writeByte(int address, byte value) {
  writeByteIfChanged(address, value);
}

byte StorageManager::readByte(int address) {
  return EEPROM.read(address);
}

void StorageManager::loadNetworkConfig(IPAddress &localIP, IPAddress &gateway, IPAddress &subnet) {
  for (int i = 0; i < 4; i++) {
    localIP[i] = EEPROM.read(EEPROM_IP_START + i);
    gateway[i] = EEPROM.read(EEPROM_GATEWAY_START + i);
    subnet[i]  = EEPROM.read(EEPROM_SUBNET_START + i);
  }
}

void StorageManager::saveNetworkConfig(const IPAddress &localIP, const IPAddress &gateway, const IPAddress &subnet) {
  for (int i = 0; i < 4; i++) {
    writeByteIfChanged(EEPROM_IP_START + i, localIP[i]);
    writeByteIfChanged(EEPROM_GATEWAY_START + i, gateway[i]);
    writeByteIfChanged(EEPROM_SUBNET_START + i, subnet[i]);
  }
}

void StorageManager::loadTallyMap(byte inputs[MAXTALLIES], byte cams[MAXTALLIES]) {
  for (int i = 0; i < MAXTALLIES; i++) {
    inputs[i] = readInt(EEPROM_TALLY_MAP_START + (i * 2));
    cams[i]   = readInt(EEPROM_CAMERA_MAP_START + (i * 2));
  }
}

void StorageManager::saveTallyMap(const byte inputs[MAXTALLIES], const byte cams[MAXTALLIES]) {
  for (int i = 0; i < MAXTALLIES; i++) {
    writeInt(EEPROM_TALLY_MAP_START + (i * 2), inputs[i]);
    writeInt(EEPROM_CAMERA_MAP_START + (i * 2), cams[i]);
  }
}

void StorageManager::loadOverrides(bool &tallyOverride, bool &ccuOverride) {
  byte tallyByte = EEPROM.read(EEPROM_TALLY_OVERRIDE_ADDR);
  byte ccuByte = EEPROM.read(EEPROM_CCU_OVERRIDE_ADDR);
  
  tallyOverride = (tallyByte == 1);
  ccuOverride = (ccuByte == 1);
}

void StorageManager::saveOverrides(bool tallyOverride, bool ccuOverride) {
  writeByteIfChanged(EEPROM_TALLY_OVERRIDE_ADDR, tallyOverride ? 1 : 0);
  writeByteIfChanged(EEPROM_CCU_OVERRIDE_ADDR, ccuOverride ? 1 : 0);
}

void StorageManager::loadOverrides(bool &tallyOverride, bool &ccuOverride, bool &vmixConnect) {
  byte tallyByte = EEPROM.read(EEPROM_TALLY_OVERRIDE_ADDR);
  byte ccuByte = EEPROM.read(EEPROM_CCU_OVERRIDE_ADDR);
  byte vmixByte = EEPROM.read(EEPROM_VMIX_CONNECT_ADDR);
  
  tallyOverride = (tallyByte == 1);
  ccuOverride = (ccuByte == 1);
  vmixConnect = (vmixByte != 0); // Assume true if never written
}

void StorageManager::saveOverrides(bool tallyOverride, bool ccuOverride, bool vmixConnect) {
  writeByteIfChanged(EEPROM_TALLY_OVERRIDE_ADDR, tallyOverride ? 1 : 0);
  writeByteIfChanged(EEPROM_CCU_OVERRIDE_ADDR, ccuOverride ? 1 : 0);
  writeByteIfChanged(EEPROM_VMIX_CONNECT_ADDR, vmixConnect ? 1 : 0);
}

void StorageManager::generateMacAddress(byte mac[6]) {
  // First 4 bytes are constant
  mac[0] = 0xA8;
  mac[1] = 0x61;
  mac[2] = 0x0A;
  mac[3] = 0xAE;

  // Read bytes 5 and 6 from EEPROM
  byte byte5 = EEPROM.read(MAC_BYTE_5_ADDRESS);
  byte byte6 = EEPROM.read(MAC_BYTE_6_ADDRESS);

  // If not initialized (0xFF), generate random values
  if (byte5 == 0xFF && byte6 == 0xFF) {
    randomSeed(analogRead(0));
    
    byte5 = random(0x00, 0xFF);
    byte6 = random(0x00, 0xFF);

    EEPROM.write(MAC_BYTE_5_ADDRESS, byte5);
    EEPROM.write(MAC_BYTE_6_ADDRESS, byte6);
  }
  
  mac[4] = byte5;
  mac[5] = byte6;
}
