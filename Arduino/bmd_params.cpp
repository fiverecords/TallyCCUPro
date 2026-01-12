/*
 * bmd_params.cpp
 * Blackmagic camera parameter table
 * Version 3.0
 * 
 * Generated from Blackmagic Camera Control Protocol documentation
 */

#include "bmd_params.h"

BMDParamDef bmdParams[] = {
  // === LENS (Group 0) ===
  { 0, 0, TYPE_FIXED16, "focus", false, 1 },
  { 0, 1, TYPE_VOID, "instantaneous_autofocus", false, 1 },
  { 0, 2, TYPE_FIXED16, "aperture_f_stop", false, 1 },
  { 0, 3, TYPE_FIXED16, "aperture_normalised", false, 1 },
  { 0, 4, TYPE_INT16, "aperture_ordinal", false, 1 },
  { 0, 5, TYPE_VOID, "instantaneous_auto_aperture", false, 1 },
  { 0, 6, TYPE_BOOLEAN, "optical_image_stabilisation", false, 1 },
  { 0, 7, TYPE_INT16, "set_absolute_zoom_mm", false, 1 },
  { 0, 8, TYPE_FIXED16, "set_absolute_zoom_normalised", false, 1 },
  { 0, 9, TYPE_FIXED16, "set_continuous_zoom_speed", false, 1 },
  
  // === VIDEO (Group 1) ===
  { 1, 0, TYPE_INT8, "video_mode", true, 5 },
  { 1, 1, TYPE_INT8, "gain", false, 1 },
  { 1, 2, TYPE_INT16, "manual_white_balance", true, 2 },
  { 1, 3, TYPE_VOID, "set_auto_wb", false, 1 },
  { 1, 4, TYPE_VOID, "restore_auto_wb", false, 1 },
  { 1, 5, TYPE_INT32, "exposure_us", false, 1 },
  { 1, 6, TYPE_INT16, "exposure_ordinal", false, 1 },
  { 1, 7, TYPE_INT8, "dynamic_range_mode", false, 1 },
  { 1, 8, TYPE_INT8, "video_sharpening_level", false, 1 },
  { 1, 10, TYPE_INT8, "set_auto_exposure_mode", false, 1 },
  { 1, 11, TYPE_INT32, "shutter_angle", false, 1 },
  { 1, 12, TYPE_INT32, "shutter_speed", false, 1 },
  { 1, 13, TYPE_INT8, "gain_db", false, 1 },
  { 1, 14, TYPE_INT32, "iso", false, 1 },
  { 1, 15, TYPE_INT8, "display_lut", true, 2 },
  { 1, 16, TYPE_FIXED16, "nd_filter_stop", true, 2 },
  
  // === AUDIO (Group 2) ===
  { 2, 0, TYPE_FIXED16, "mic_level", false, 1 },
  { 2, 1, TYPE_FIXED16, "headphone_level", false, 1 },
  { 2, 2, TYPE_FIXED16, "headphone_program_mix", false, 1 },
  { 2, 3, TYPE_FIXED16, "speaker_level", false, 1 },
  { 2, 4, TYPE_INT8, "input_type", false, 1 },
  { 2, 5, TYPE_FIXED16, "input_levels", true, 2 },
  { 2, 6, TYPE_BOOLEAN, "phantom_power", false, 1 },
  
  // === OUTPUT (Group 3) ===
  { 3, 3, TYPE_INT8, "overlays", true, 4 },
  
  // === DISPLAY (Group 4) ===
  { 4, 0, TYPE_FIXED16, "brightness", false, 1 },
  { 4, 1, TYPE_UINT16, "exposure_and_focus_tools", true, 2 },
  { 4, 2, TYPE_FIXED16, "zebra_level", false, 1 },
  { 4, 3, TYPE_FIXED16, "peaking_level", false, 1 },
  { 4, 4, TYPE_INT8, "color_bars_display_time_seconds", false, 1 },
  { 4, 5, TYPE_INT8, "focus_assist", true, 2 },
  { 4, 6, TYPE_INT8, "program_return_feed_enable", false, 1 },
  { 4, 7, TYPE_INT8, "timecode_source", false, 1 },
  
  // === TALLY (Group 5) ===
  { 5, 0, TYPE_FIXED16, "tally_brightness", false, 1 },
  { 5, 1, TYPE_FIXED16, "front_tally_brightness", false, 1 },
  { 5, 2, TYPE_FIXED16, "rear_tally_brightness", false, 1 },
  
  // === REFERENCE (Group 6) ===
  { 6, 0, TYPE_INT8, "source", false, 1 },
  { 6, 1, TYPE_INT32, "offset", false, 1 },
  
  // === CONFIGURATION (Group 7) ===
  { 7, 0, TYPE_INT32, "real_time_clock", true, 2 },
  { 7, 1, TYPE_STRING, "system_language", false, 1 },
  { 7, 2, TYPE_INT32, "timezone", false, 1 },
  { 7, 3, TYPE_INT64, "location", true, 2 },
  
  // === COLOR CORRECTION (Group 8) ===
  { 8, 0, TYPE_FIXED16, "lift_adjust", true, 4 },
  { 8, 1, TYPE_FIXED16, "gamma_adjust", true, 4 },
  { 8, 2, TYPE_FIXED16, "gain_adjust", true, 4 },
  { 8, 3, TYPE_FIXED16, "offset_adjust", true, 4 },
  { 8, 4, TYPE_FIXED16, "contrast_adjust", true, 2 },
  { 8, 5, TYPE_FIXED16, "luma_mix", false, 1 },
  { 8, 6, TYPE_FIXED16, "color_adjust", true, 2 },
  { 8, 7, TYPE_VOID, "correction_reset_default", false, 1 },
  
  // === MEDIA (Group 10) ===
  { 10, 0, TYPE_INT8, "codec", true, 2 },
  { 10, 1, TYPE_INT8, "transport_mode", true, 5 },
  
  // === PTZ CONTROL (Group 11) ===
  { 11, 0, TYPE_FIXED16, "pan_tilt_velocity", true, 2 },
  { 11, 1, TYPE_INT8, "memory_preset", true, 2 }
};

const int BMD_PARAM_COUNT = sizeof(bmdParams)/sizeof(bmdParams[0]);
