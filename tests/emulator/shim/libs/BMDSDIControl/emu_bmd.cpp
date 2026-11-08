#include "BMDSDIControl.h"
#include "emu_internal.h"
#include <time.h>
static unsigned long long wall_ms() { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (unsigned long long)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000; }

static int g_fd = -2;
void emu_bmd_log(const char *fn, int cam, int g, int p, int op, const char *vals, int n) {
  if (g_fd == -2) g_fd = emu_open_log("EMU_BMD_LOG");
  if (g_fd < 0) return;
  char buf[900];
  int k = snprintf(buf, sizeof buf,
                   "{\"wt\":%llu,\"t\":%u,\"boot\":%d,\"fn\":\"%s\",\"cam\":%d,\"group\":%d,\"param\":%d,\"op\":%d,\"n\":%d,\"v\":%s}\n",
                   wall_ms(), emu_millis32(), emu_boot_count(), fn, cam, g, p, op, n, vals);
  if (k > (int)sizeof buf - 1) { k = sizeof buf - 2; buf[k++] = '\n'; }
  emu_write_line(g_fd, buf, (size_t)k);
}
void emu_bmd_fmt_f(char *out, size_t sz, const float *v, int n, bool fixed) {
  size_t k = 0;
  k += snprintf(out + k, sz - k, "[");
  for (int i = 0; i < n && k < sz - 40; i++) {
    float f = v[i];
    if (fixed) {
      long long raw = (long long)(f * 2048.0f);
      int16_t enc = (int16_t)(uint16_t)(raw & 0xFFFF);
      if (f != f) k += snprintf(out + k, sz - k, "%s{\"f\":\"nan\",\"raw\":0}", i ? "," : "");
      else if (isinf(f)) k += snprintf(out + k, sz - k, "%s{\"f\":\"%sinf\",\"raw\":0}", i ? "," : "", f < 0 ? "-" : "");
      else k += snprintf(out + k, sz - k, "%s{\"f\":%.9g,\"raw\":%d}", i ? "," : "", (double)f, (int)enc);
    } else {
      k += snprintf(out + k, sz - k, "%s%.9g", i ? "," : "", (double)f);
    }
  }
  snprintf(out + k, sz - k, "]");
}
void emu_bmd_fmt_i(char *out, size_t sz, const long long *v, int n) {
  size_t k = 0;
  k += snprintf(out + k, sz - k, "[");
  for (int i = 0; i < n && k < sz - 24; i++) k += snprintf(out + k, sz - k, "%s%lld", i ? "," : "", v[i]);
  snprintf(out + k, sz - k, "]");
}

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
// EMU_SHIELD_FILE: first byte '0' = shield absent (begin() hangs, I2C NACK)
bool emu_shield_present() {
  const char *f = getenv("EMU_SHIELD_FILE");
  if (!f) return true;
  FILE *fp = fopen(f, "r"); if (!fp) return true;
  int c = fgetc(fp); fclose(fp);
  return c != '0';
}
void emu_shield_hang() { usleep(2000); }

// EMU_HANG_FILE: first byte '1' = every tally write hangs (I2C stuck in
// operation) until it changes
bool emu_op_hang() {
  const char *f = getenv("EMU_HANG_FILE");
  if (!f) return false;
  FILE *fp = fopen(f, "r"); if (!fp) return false;
  int c = fgetc(fp); fclose(fp);
  return c == '1';
}

// EMU_TALLY_FAIL_FILE '1': setCameraTally() returns false (write not ACKed)
// EMU_SHIELD_RESET_FILE '1': the shield lost its settings; getOverride()
// reads false until setOverride() is called again (which writes '0')
static int file_flag(const char *env) {
  const char *f = getenv(env);
  if (!f) return 0;
  FILE *fp = fopen(f, "r"); if (!fp) return 0;
  int c = fgetc(fp); fclose(fp);
  return c == '1';
}
bool emu_tally_fail() { return file_flag("EMU_TALLY_FAIL_FILE"); }
bool emu_shield_reset_pending() { return file_flag("EMU_SHIELD_RESET_FILE"); }
void emu_shield_reset_clear() {
  const char *f = getenv("EMU_SHIELD_RESET_FILE");
  if (!f) return;
  FILE *fp = fopen(f, "w"); if (fp) { fputc('0', fp); fclose(fp); }
}
