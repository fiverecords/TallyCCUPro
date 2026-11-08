// TallyManager: vMix input -> camera map slots.
#include "test.h"
#include "TallyManager.h"

static void slot(byte i, byte &in, byte &cam) {
  in = 99;
  cam = 99;
  TallyManager::getMapping(i, in, cam);
}

TEST(tally_map_set_get_clear) {
  for (byte i = 0; i < MAXTALLIES; i++) TallyManager::clearMapping(i);
  TallyManager::takeReapply();
  TallyManager::setMapping(0, 1, 1);
  TallyManager::setMapping(1, MAX_VMIX_INPUTS, MAX_CAMERAS);
  CHECK(TallyManager::takeReapply());  // the vMix state is re-applied once
  CHECK(!TallyManager::takeReapply());
  byte in, cam;
  slot(0, in, cam);
  CHECK_INT(in, 1);
  CHECK_INT(cam, 1);
  slot(1, in, cam);
  CHECK_INT(in, MAX_VMIX_INPUTS);
  CHECK_INT(cam, MAX_CAMERAS);
  slot(2, in, cam);
  CHECK_INT(in, 0);
  CHECK_INT(cam, 0);
  TallyManager::clearMapping(1);
  CHECK(TallyManager::takeReapply());
  slot(1, in, cam);
  CHECK_INT(in, 0);
  CHECK_INT(cam, 0);
}

TEST(tally_map_rejects_half_and_out_of_range) {
  for (byte i = 0; i < MAXTALLIES; i++) TallyManager::clearMapping(i);
  byte in, cam;
  TallyManager::setMapping(2, 4, 0);  // half-set slot is stored empty (TAL-07)
  slot(2, in, cam);
  CHECK_INT(in, 0);
  CHECK_INT(cam, 0);
  TallyManager::setMapping(2, 4, MAX_CAMERAS + 1);  // camera out of range
  slot(2, in, cam);
  CHECK_INT(in, 0);
  CHECK_INT(cam, 0);
  TallyManager::setMapping(2, MAX_VMIX_INPUTS + 1, 1);  // input out of range
  slot(2, in, cam);
  CHECK_INT(in, 0);
  CHECK_INT(cam, 0);
  TallyManager::setMapping(MAXTALLIES, 4, 1);  // slot out of range: ignored
  slot(MAXTALLIES, in, cam);
  CHECK_INT(in, 0);
  CHECK_INT(cam, 0);
  TallyManager::setMapping(3, 4, 2);
  TallyManager::setMapping(3, 0, 0);  // 0 leaves each half unchanged
  slot(3, in, cam);
  CHECK_INT(in, 4);
  CHECK_INT(cam, 2);
  TallyManager::setMapping(3, 0, 5);  // change only the camera
  slot(3, in, cam);
  CHECK_INT(in, 4);
  CHECK_INT(cam, 5);
  TallyManager::clearMapping(3);
}

TEST(tally_map_survives_reload) {
  for (byte i = 0; i < MAXTALLIES; i++) TallyManager::clearMapping(i);
  TallyManager::setMapping(4, 7, 3);
  TallyManager::loadMap();  // back from the (emulated) EEPROM
  byte in, cam;
  slot(4, in, cam);
  CHECK_INT(in, 7);
  CHECK_INT(cam, 3);
  TallyManager::clearMapping(4);
  TallyManager::loadMap();
  slot(4, in, cam);
  CHECK_INT(in, 0);
  CHECK_INT(cam, 0);
}
