// CCUControl number/boolean parsers and the parameter table lookups.
#include "test.h"
#include "CCUControl.h"
#include "bmd_params.h"

TEST(parse_float_list_single_and_lists) {
  float v[5];
  CHECK_INT(CCUControl::parseFloatList("0.5", v, 1), 1);
  CHECK_NEAR(v[0], 0.5, 1e-7);
  CHECK_INT(CCUControl::parseFloatList("-2", v, 1), 1);
  CHECK_NEAR(v[0], -2.0, 1e-7);
  CHECK_INT(CCUControl::parseFloatList("1,2.5,-3,0.25", v, 4), 4);
  CHECK_NEAR(v[1], 2.5, 1e-7);
  CHECK_NEAR(v[2], -3.0, 1e-7);
  CHECK_NEAR(v[3], 0.25, 1e-7);
  CHECK_INT(CCUControl::parseFloatList(" 1 , 2 ", v, 2), 2);  // spaces around tokens
  CHECK_NEAR(v[1], 2.0, 1e-7);
}

TEST(parse_float_list_zero_fills) {
  float v[4] = {9, 9, 9, 9};
  CHECK_INT(CCUControl::parseFloatList("1", v, 4), 1);
  CHECK_NEAR(v[1], 0.0, 0);
  CHECK_NEAR(v[3], 0.0, 0);
}

TEST(parse_float_list_companion_values) {
  // Unrounded floats and exponents sent by the Companion module (CCU-01)
  float v[4];
  CHECK_INT(CCUControl::parseFloatList("0.30000000000000004", v, 1), 1);
  CHECK_NEAR(v[0], 0.3, 1e-6);
  CHECK_INT(CCUControl::parseFloatList("0.5,1.3877787807814457e-16", v, 2), 2);
  CHECK_NEAR(v[1], 0.0, 1e-12);
  CHECK_INT(CCUControl::parseFloatList("1e-3", v, 1), 1);
  CHECK_NEAR(v[0], 0.001, 1e-9);
  CHECK_INT(CCUControl::parseFloatList("-1.6999999999999997,0.30000000000000004,0,1", v, 4), 4);
  CHECK_NEAR(v[0], -1.7, 1e-6);
}

TEST(parse_float_list_empty_list_items) {
  // "," is what the module sends for exposure_and_focus_tools defaults
  float v[2] = {7, 7};
  CHECK_INT(CCUControl::parseFloatList(",", v, 2), 2);
  CHECK_NEAR(v[0], 0.0, 0);
  CHECK_NEAR(v[1], 0.0, 0);
  CHECK_INT(CCUControl::parseFloatList("1,", v, 2), 2);
  CHECK_NEAR(v[0], 1.0, 0);
}

TEST(parse_float_list_rejects_malformed) {
  float v[4];
  CHECK_INT(CCUControl::parseFloatList("", v, 1), -1);
  CHECK_INT(CCUControl::parseFloatList("   ", v, 1), -1);
  CHECK_INT(CCUControl::parseFloatList("abc", v, 1), -1);
  CHECK_INT(CCUControl::parseFloatList("1x", v, 1), -1);
  CHECK_INT(CCUControl::parseFloatList("1;2", v, 2), -1);
  CHECK_INT(CCUControl::parseFloatList("nan", v, 1), -1);
  CHECK_INT(CCUControl::parseFloatList("inf", v, 1), -1);
  CHECK_INT(CCUControl::parseFloatList("1,2,3", v, 2), -1);  // too many values
  CHECK_INT(CCUControl::parseFloatList(NULL, v, 1), -1);
  CHECK_INT(CCUControl::parseFloatList("1", NULL, 1), -1);
  CHECK_INT(CCUControl::parseFloatList("1", v, 0), -1);
}

TEST(parse_bool_words_and_numbers) {
  CHECK_INT(CCUControl::parseBool("1"), 1);
  CHECK_INT(CCUControl::parseBool("0"), 0);
  CHECK_INT(CCUControl::parseBool("true"), 1);
  CHECK_INT(CCUControl::parseBool("TRUE"), 1);
  CHECK_INT(CCUControl::parseBool("On"), 1);
  CHECK_INT(CCUControl::parseBool("yes"), 1);
  CHECK_INT(CCUControl::parseBool("false"), 0);
  CHECK_INT(CCUControl::parseBool("OFF"), 0);
  CHECK_INT(CCUControl::parseBool("no"), 0);
  CHECK_INT(CCUControl::parseBool("  true  "), 1);
  CHECK_INT(CCUControl::parseBool("2"), 1);
  CHECK_INT(CCUControl::parseBool("-1"), 1);
  CHECK_INT(CCUControl::parseBool("0.0"), 0);
}

TEST(parse_bool_rejects) {
  CHECK_INT(CCUControl::parseBool(NULL), -1);
  CHECK_INT(CCUControl::parseBool(""), -1);
  CHECK_INT(CCUControl::parseBool("abc"), -1);
  CHECK_INT(CCUControl::parseBool("truex"), -1);
  CHECK_INT(CCUControl::parseBool("yes please"), -1);
  CHECK_INT(CCUControl::parseBool("1,0"), -1);
}

TEST(param_table_lookup) {
  BMDParamDef d;
  CHECK(bmdFindParam("focus", &d) >= 0);
  CHECK_INT(d.groupId, 0);
  CHECK_INT(d.paramId, 0);
  CHECK_INT(d.paramType, TYPE_FIXED16);
  CHECK_INT(d.subCount, 1);
  CHECK(bmdFindParam("manual_white_balance", &d) >= 0);
  CHECK_INT(d.paramType, TYPE_INT16);
  CHECK_INT(d.subCount, 2);
  CHECK(bmdFindParam("lift_adjust", &d) >= 0);
  CHECK_INT(d.groupId, 8);
  CHECK_INT(d.subCount, 4);
  CHECK(bmdFindParam("no_such_param", &d) < 0);
  CHECK(bmdFindParam("", &d) < 0);
  CHECK(bmdFindParam(NULL, &d) < 0);
  CHECK(bmdFindParam("focusx", &d) < 0);
  CHECK(bmdFindParam("focu", &d) < 0);
}

TEST(param_table_key0_alias) {
  // "<key>_0" is accepted for single-value keys only
  BMDParamDef d;
  CHECK(bmdFindParam("gain_db_0", &d) >= 0);
  CHECK_INT(d.groupId, 1);
  CHECK_INT(d.paramId, 13);
  CHECK(bmdFindParam("lift_adjust_0", &d) < 0);
  const BMDParamDef* p = CCUControl::findParamDefByKey("focus_0");
  CHECK(p != NULL);
  CHECK(CCUControl::findParamDefByKey("not_a_key") == NULL);
}

TEST(param_table_void_and_rows) {
  CHECK(bmdIsVoidParam("instantaneous_autofocus"));
  CHECK(bmdIsVoidParam("set_auto_wb"));
  CHECK(!bmdIsVoidParam("focus"));
  CHECK(!bmdIsVoidParam(NULL));
  CHECK(BMD_PARAM_COUNT > 50);
  // every list fits the fixed-size arrays used to write it
  BMDParamDef d;
  const char* keys[] = {"video_mode", "manual_white_balance", "lift_adjust", "gamma_adjust",
                        "gain_adjust", "offset_adjust", "contrast_adjust", "color_adjust"};
  for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
    CHECK(bmdFindParam(keys[i], &d) >= 0);
    CHECK(d.subCount >= 2 && d.subCount <= BMD_MAX_SUB_COUNT);
  }
}
