// NetworkManager address validators and the factory network settings
// (StorageManager) on a blank EEPROM.
#include "test.h"
#include "Network.h"
#include "Storage.h"

static bool ip(const char* s, byte a, byte b, byte c, byte d) {
  byte o[4] = {9, 9, 9, 9};
  return NetworkManager::parseIP(s, o) && o[0] == a && o[1] == b && o[2] == c && o[3] == d;
}

static bool bad(const char* s) {
  byte o[4];
  return !NetworkManager::parseIP(s, o);
}

static bool host(byte a, byte b, byte c, byte d) {
  const byte v[4] = {a, b, c, d};
  return NetworkManager::isHostIP(v);
}

static bool mask(byte a, byte b, byte c, byte d) {
  const byte v[4] = {a, b, c, d};
  return NetworkManager::isSubnetMask(v);
}

static bool same(const IPAddress& v, byte a, byte b, byte c, byte d) {
  return v[0] == a && v[1] == b && v[2] == c && v[3] == d;
}

static void blankEeprom() {
  for (int i = 0; i <= E2END; i++) EEPROM.update(i, 0xFF);
}

TEST(parse_ip_strict) {
  CHECK(ip("192.168.0.100", 192, 168, 0, 100));
  CHECK(ip("0.0.0.0", 0, 0, 0, 0));
  CHECK(ip("255.255.255.255", 255, 255, 255, 255));
  CHECK(ip("010.001.000.009", 10, 1, 0, 9));  // leading zeros are decimal
  CHECK(bad(""));
  CHECK(bad(NULL));
  CHECK(bad("192.168.0"));
  CHECK(bad("192.168.0.1.5"));
  CHECK(bad("192.168.0.256"));
  CHECK(bad("1920.168.0.1"));
  CHECK(bad("0192.168.0.1"));  // 4 digits
  CHECK(bad("192.168..1"));
  CHECK(bad(" 192.168.0.1"));
  CHECK(bad("192.168.0.1 "));
  CHECK(bad("192.168.0.1\r"));
  CHECK(bad("192.168.0.-1"));
  CHECK(bad("192.168.0.x"));
  IPAddress a;
  CHECK(NetworkManager::parseIP("10.0.0.7", a) && same(a, 10, 0, 0, 7));
  CHECK(!NetworkManager::parseIP("10.0.0", a));
}

TEST(host_ip_classes) {
  CHECK(host(192, 168, 0, 100));
  CHECK(host(1, 0, 0, 1));
  CHECK(host(10, 0, 0, 1));
  CHECK(host(223, 255, 255, 254));
  CHECK(!host(0, 1, 2, 3));        // "this network"
  CHECK(!host(127, 0, 0, 1));      // loopback
  CHECK(!host(224, 0, 0, 1));      // multicast
  CHECK(!host(255, 255, 255, 255));
}

TEST(subnet_mask_contiguous) {
  CHECK(mask(255, 255, 255, 0));
  CHECK(mask(255, 255, 0, 0));
  CHECK(mask(255, 0, 0, 0));
  CHECK(mask(128, 0, 0, 0));        // /1
  CHECK(mask(255, 255, 255, 252));  // /30
  CHECK(!mask(255, 255, 255, 254)); // /31: no host addresses
  CHECK(!mask(255, 255, 255, 255)); // /32
  CHECK(!mask(0, 0, 0, 0));
  CHECK(!mask(255, 0, 255, 0));     // not contiguous
  CHECK(!mask(0, 255, 255, 255));
}

// README "Default settings" (DOC-08): a blank EEPROM boots as
// 192.168.0.100/24, gateway .1, vMix 192.168.0.140, overrides ON.
TEST(factory_defaults_on_blank_eeprom) {
  blankEeprom();
  StorageManager::begin();
  IPAddress local, gw, sub;
  StorageManager::loadNetworkConfig(local, gw, sub);
  CHECK(same(local, 192, 168, 0, 100));
  CHECK(same(gw, 192, 168, 0, 1));
  CHECK(same(sub, 255, 255, 255, 0));
  byte v[4];
  StorageManager::loadVmixIP(v);
  CHECK(v[0] == 192 && v[1] == 168 && v[2] == 0 && v[3] == 140);
  bool t = false, c = false, m = false;
  StorageManager::loadOverrides(t, c, m);
  CHECK(t && c && m);
  byte ins[MAXTALLIES], cams[MAXTALLIES];
  StorageManager::loadTallyMap(ins, cams);
  int used = 0;
  for (int i = 0; i < MAXTALLIES; i++) used += (ins[i] != 0) + (cams[i] != 0);
  CHECK_INT(used, 0);
}

// A first octet of 0 or 255 reads as "never written": the default is used
TEST(first_octet_0_or_255_is_unset) {
  StorageManager::saveNetworkConfig(IPAddress(0, 1, 2, 3), IPAddress(0, 1, 2, 1), IPAddress(255, 255, 0, 0));
  IPAddress local, gw, sub;
  StorageManager::loadNetworkConfig(local, gw, sub);
  CHECK(same(local, 192, 168, 0, 100));
  CHECK(same(sub, 255, 255, 255, 0));
  const byte v255[4] = {255, 1, 2, 3};
  StorageManager::saveVmixIP(v255);
  byte v[4];
  StorageManager::loadVmixIP(v);
  CHECK(v[0] == 192 && v[1] == 168 && v[2] == 0 && v[3] == 140);
  const byte vok[4] = {10, 0, 0, 20};
  StorageManager::saveVmixIP(vok);
  StorageManager::loadVmixIP(v);
  CHECK(v[0] == 10 && v[1] == 0 && v[2] == 0 && v[3] == 20);
}

// Settings stored by an older firmware (no layout header) are kept
TEST(settings_without_header_kept) {
  blankEeprom();
  StorageManager::saveNetworkConfig(IPAddress(10, 1, 2, 3), IPAddress(10, 1, 2, 254), IPAddress(255, 255, 255, 0));
  StorageManager::saveOverrides(false, true, false);
  StorageManager::begin();
  IPAddress local, gw, sub;
  StorageManager::loadNetworkConfig(local, gw, sub);
  CHECK(same(local, 10, 1, 2, 3));
  CHECK(same(gw, 10, 1, 2, 254));
  bool t = true, c = false, m = true;
  StorageManager::loadOverrides(t, c, m);
  CHECK(!t && c && !m);
}

TEST(factory_reset_keeps_mac) {
  const byte mac[6] = {0x02, 0x54, 0x00, 0x12, 0x34, 0x56};
  CHECK(StorageManager::saveMacAddress(mac));
  const byte multicast[6] = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01};
  CHECK(!StorageManager::saveMacAddress(multicast));
  StorageManager::saveNetworkConfig(IPAddress(10, 9, 8, 7), IPAddress(10, 9, 8, 1), IPAddress(255, 255, 0, 0));
  StorageManager::factoryReset();
  IPAddress local, gw, sub;
  StorageManager::loadNetworkConfig(local, gw, sub);
  CHECK(same(local, 192, 168, 0, 100));
  byte got[6] = {0};
  StorageManager::generateMacAddress(got);
  CHECK(memcmp(got, mac, 6) == 0);
}
