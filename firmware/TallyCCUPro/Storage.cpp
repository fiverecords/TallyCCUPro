/*
 * Storage.cpp
 * EEPROM storage implementation
 *
 * Every write goes through EEPROM.update(): a cell is only written when its
 * value changes (each cell is good for ~100,000 writes).
 */

#include "Storage.h"
#include "SafeMode.h"

static bool blankCell16(int address) {
  return EEPROM.read(address) == 0xFF && EEPROM.read(address + 1) == 0xFF;
}

void StorageManager::begin() {
  byte ver = EEPROM.read(EEPROM_HEADER_ADDR + 2);
  bool header = EEPROM.read(EEPROM_HEADER_ADDR) == EEPROM_MAGIC0 &&
                EEPROM.read(EEPROM_HEADER_ADDR + 1) == EEPROM_MAGIC1 &&
                EEPROM.read(EEPROM_HEADER_ADDR + 3) == (byte)~ver;
  if (header && ver == EEPROM_LAYOUT_VERSION) return;
  if (header && ver > EEPROM_LAYOUT_VERSION) {
    // Written by a newer firmware: used as is, never rewritten
    Serial.println(F("EEPROM: layout from a newer firmware, kept as is"));
    return;
  }

  // No header: blank EEPROM, or the map of v3.7.1/v3.8 (same addresses).
  // Fill in only the cells those versions treat as never written, with the
  // value they would use, so a unit in the field keeps every setting.
  byte ip0 = EEPROM.read(EEPROM_IP_START);
  bool blank = (ip0 == 0 || ip0 == 255);
  if (blank) writeNetworkDefaults();

  byte v0 = (byte)readInt(EEPROM_VMIX_IP0);
  if (v0 == 0 || v0 == 255) writeVmixDefault();

  // Overrides: 0xFF already reads as ON
  if (EEPROM.read(EEPROM_TALLY_OVERRIDE_ADDR) == 0xFF) writeByteIfChanged(EEPROM_TALLY_OVERRIDE_ADDR, 1);
  if (EEPROM.read(EEPROM_CCU_OVERRIDE_ADDR) == 0xFF) writeByteIfChanged(EEPROM_CCU_OVERRIDE_ADDR, 1);
  if (EEPROM.read(EEPROM_VMIX_CONNECT_ADDR) == 0xFF) writeByteIfChanged(EEPROM_VMIX_CONNECT_ADDR, 1);

  // Tally slots never written (both halves 0xFFFF) are empty already
  for (int i = 0; i < MAXTALLIES; i++) {
    int in = EEPROM_TALLY_MAP_START + i * 2;
    int cam = EEPROM_CAMERA_MAP_START + i * 2;
    if (blankCell16(in) && blankCell16(cam)) {
      writeInt(in, 0);
      writeInt(cam, 0);
    }
  }

  // Header last: an interrupted migration simply runs again
  writeByteIfChanged(EEPROM_HEADER_ADDR, EEPROM_MAGIC0);
  writeByteIfChanged(EEPROM_HEADER_ADDR + 1, EEPROM_MAGIC1);
  writeByteIfChanged(EEPROM_HEADER_ADDR + 2, EEPROM_LAYOUT_VERSION);
  writeByteIfChanged(EEPROM_HEADER_ADDR + 3, (byte)~EEPROM_LAYOUT_VERSION);

  Serial.println(blank ? F("EEPROM: no stored IP, defaults written where unset")
                       : F("EEPROM: settings from a previous version kept"));
}

void StorageManager::factoryReset() {
  writeNetworkDefaults();
  writeVmixDefault();
  saveOverrides(true, true, true);
  for (int i = 0; i < MAXTALLIES; i++) {
    writeInt(EEPROM_TALLY_MAP_START + i * 2, 0);
    writeInt(EEPROM_CAMERA_MAP_START + i * 2, 0);
  }
  writeByteIfChanged(EEPROM_SAFEMODE_MANUAL_ADDR, 0);
  writeByteIfChanged(EEPROM_SAFEMODE_STRIKES_ADDR, 0);
  writeByteIfChanged(EEPROM_SAFEMODE_AUTO_ADDR, 0);
  Serial.println(F("EEPROM: factory settings written (MAC kept)"));
}

void StorageManager::writeNetworkDefaults() {
  saveNetworkConfig(IPAddress(DEFAULT_LOCAL_IP), IPAddress(DEFAULT_GATEWAY),
                    IPAddress(DEFAULT_SUBNET));
}

void StorageManager::writeVmixDefault() {
  const byte ip[4] = {DEFAULT_VMIX_IP};
  saveVmixIP(ip);
}

// Write byte only if changed
void StorageManager::writeByteIfChanged(int address, byte value) {
  EEPROM.update(address, value);
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

  // Protect against uninitialized EEPROM (all 0xFF or all 0x00).
  // Factory default 192.168.0.100/24; the default vMix IP is in the same /24.
  if (localIP[0] == 0 || localIP[0] == 255) {
    localIP = IPAddress(DEFAULT_LOCAL_IP);
    gateway = IPAddress(DEFAULT_GATEWAY);
    subnet  = IPAddress(DEFAULT_SUBNET);
    saveNetworkConfig(localIP, gateway, subnet);
    Serial.println(F("Network config initialized to defaults"));
  }
}

void StorageManager::saveNetworkConfig(const IPAddress &localIP, const IPAddress &gateway, const IPAddress &subnet) {
  for (int i = 0; i < 4; i++) {
    writeByteIfChanged(EEPROM_IP_START + i, localIP[i]);
    writeByteIfChanged(EEPROM_GATEWAY_START + i, gateway[i]);
    writeByteIfChanged(EEPROM_SUBNET_START + i, subnet[i]);
  }
}

void StorageManager::loadVmixIP(byte ip[4]) {
  ip[0] = readInt(EEPROM_VMIX_IP0);
  ip[1] = readInt(EEPROM_VMIX_IP1);
  ip[2] = readInt(EEPROM_VMIX_IP2);
  ip[3] = readInt(EEPROM_VMIX_IP3);

  // Use defaults if not initialized (same /24 as the default local IP)
  if (ip[0] == 0 || ip[0] == 255) {
    writeVmixDefault();
    const byte def[4] = {DEFAULT_VMIX_IP};
    memcpy(ip, def, 4);
    Serial.println(F("vMix IP initialized to defaults"));
  }
}

void StorageManager::saveVmixIP(const byte ip[4]) {
  writeInt(EEPROM_VMIX_IP0, ip[0]);
  writeInt(EEPROM_VMIX_IP1, ip[1]);
  writeInt(EEPROM_VMIX_IP2, ip[2]);
  writeInt(EEPROM_VMIX_IP3, ip[3]);
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

  // 0xFF (never written) => ON, same as vmixConnect
  tallyOverride = (tallyByte != 0);
  ccuOverride = (ccuByte != 0);
}

void StorageManager::saveOverrides(bool tallyOverride, bool ccuOverride) {
  writeByteIfChanged(EEPROM_TALLY_OVERRIDE_ADDR, tallyOverride ? 1 : 0);
  writeByteIfChanged(EEPROM_CCU_OVERRIDE_ADDR, ccuOverride ? 1 : 0);
}

void StorageManager::loadOverrides(bool &tallyOverride, bool &ccuOverride, bool &vmixConnect) {
  byte tallyByte = EEPROM.read(EEPROM_TALLY_OVERRIDE_ADDR);
  byte ccuByte = EEPROM.read(EEPROM_CCU_OVERRIDE_ADDR);
  byte vmixByte = EEPROM.read(EEPROM_VMIX_CONNECT_ADDR);

  // 0xFF (never written) => ON for all three
  tallyOverride = (tallyByte != 0);
  ccuOverride = (ccuByte != 0);
  vmixConnect = (vmixByte != 0);
}

void StorageManager::saveOverrides(bool tallyOverride, bool ccuOverride, bool vmixConnect) {
  writeByteIfChanged(EEPROM_TALLY_OVERRIDE_ADDR, tallyOverride ? 1 : 0);
  writeByteIfChanged(EEPROM_CCU_OVERRIDE_ADDR, ccuOverride ? 1 : 0);
  writeByteIfChanged(EEPROM_VMIX_CONNECT_ADDR, vmixConnect ? 1 : 0);
}

static bool unicastMac(const byte mac[6]) {
  if (mac[0] & 0x01) return false;   // multicast/broadcast
  byte any = 0;
  for (byte i = 0; i < 6; i++) any |= mac[i];
  return any != 0;
}

// Entropy for a new MAC (BOOT-15): boot seed (power-up RAM noise), ADC noise
// of unused inputs A8-A15 and timing jitter, mixed with FNV-1a
static uint32_t macEntropy() {
  uint32_t h = 2166136261UL ^ SafeMode::getBootSeed();
  for (byte i = 0; i < 32; i++) {
    h = (h ^ (uint16_t)analogRead(A8 + (i & 7))) * 16777619UL;
    h = (h ^ (uint32_t)micros()) * 16777619UL;
  }
  return h;
}

void StorageManager::generateMacAddress(byte mac[6]) {
  // Full MAC set with "mac" or generated by v3.8+
  if (EEPROM.read(EEPROM_MAC_MODE_ADDR) == EEPROM_MAC_FULL) {
    for (byte i = 0; i < 6; i++) mac[i] = EEPROM.read(EEPROM_MAC_START + i);
    if (unicastMac(mac)) return;
  }

  // Legacy MAC (v3.7.1/v3.8 units keep theirs): A8:61:0A:AE + 2 stored bytes
  byte byte5 = EEPROM.read(MAC_BYTE_5_ADDRESS);
  byte byte6 = EEPROM.read(MAC_BYTE_6_ADDRESS);
  if (byte5 != 0xFF || byte6 != 0xFF) {
    mac[0] = 0xA8;
    mac[1] = 0x61;
    mac[2] = 0x0A;
    mac[3] = 0xAE;
    mac[4] = byte5;
    mac[5] = byte6;
    return;
  }

  // New unit: locally administered unicast MAC 02:54:xx:xx:xx:xx
  uint32_t r = macEntropy();
  mac[0] = 0x02;
  mac[1] = 0x54;
  for (byte i = 2; i < 6; i++) {
    mac[i] = (byte)r;
    r >>= 8;
  }
  saveMacAddress(mac);
}

bool StorageManager::saveMacAddress(const byte mac[6]) {
  if (!unicastMac(mac)) return false;
  for (byte i = 0; i < 6; i++) writeByteIfChanged(EEPROM_MAC_START + i, mac[i]);
  writeByteIfChanged(EEPROM_MAC_MODE_ADDR, EEPROM_MAC_FULL);   // last
  return true;
}
