// CCUControl::validateValue(): the rules applied to client state (CCUSYNC,
// syncState) and to every parameter written to a camera.
#include "test.h"
#include "CCUControl.h"
#include "bmd_params.h"

static bool valid(const char* key, const char* value) {
  BMDParamDef d;
  if (bmdFindParam(key, &d) < 0) {
    printf("    unknown key %s\n", key);
    return false;
  }
  return CCUControl::validateValue(d, value);
}

TEST(validate_numbers) {
  CHECK(valid("focus", "0.5"));
  CHECK(valid("focus", "0.30000000000000004"));
  CHECK(valid("gain_db", "6"));
  CHECK(valid("gain_db", "-2"));
  CHECK(valid("manual_white_balance", "5600,10"));
  CHECK(valid("lift_adjust", "0.1,-0.1,0.1,0"));
  CHECK(valid("contrast_adjust", "0.5,1.3877787807814457e-16"));
  CHECK(!valid("focus", ""));
  CHECK(!valid("focus", "abc"));
  CHECK(!valid("gain_db", "6x"));
  CHECK(!valid("manual_white_balance", "1,2,3"));  // more values than subCount
}

TEST(validate_booleans_and_void) {
  CHECK(valid("optical_image_stabilisation", "1"));
  CHECK(valid("optical_image_stabilisation", "true"));
  CHECK(valid("optical_image_stabilisation", "false"));
  CHECK(!valid("optical_image_stabilisation", "maybe"));
  CHECK(valid("set_auto_wb", "1"));
}

TEST(validate_exposure_and_focus_tools_defaults) {
  // The Companion module sends "," for this action's defaults (REG-1)
  CHECK(valid("exposure_and_focus_tools", ","));
  CHECK(valid("exposure_and_focus_tools", "1,4"));
}

TEST(validate_strings_and_control_chars) {
  CHECK(valid("system_language", "en"));
  CHECK(!valid("system_language", ""));
  CHECK(!valid("system_language", "ABCDEFGHIJKLMNOPQRSTUVWXYZ"));  // > BMD_STRING_MAX
  CHECK(!valid("focus", "0.5\r"));
  CHECK(!valid("focus", "0.5\n"));
  CHECK(!valid("system_language", "e\x7f"));
  BMDParamDef d;
  CHECK(bmdFindParam("focus", &d) >= 0);
  CHECK(!CCUControl::validateValue(d, NULL));
}
