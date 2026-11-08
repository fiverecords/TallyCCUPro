/* emu_eeprom.cpp - avr/eeprom.h on top of a 4096-byte file (EMU_EEPROM).
 *
 * The file is mmap'ed MAP_SHARED, so every write is in the page cache at
 * once and survives the emulated watchdog reset (execv) and crashes, like
 * a real EEPROM. A missing/short file is created and filled with 0xFF.
 * EMU_EEPROM_WRITE_US (default 3400 in EMU_REALTIME mode) emulates the
 * ~3.4 ms programming time: an access waits for the previous write.
 * EMU_EEPROM_WEAR_WARN (default 1000): warn once when one cell has been
 * written that many times in this boot. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include "emu_internal.h"
#include "avr/eeprom.h"

#define EE_SIZE 4096

static uint8_t *g_ee;
static uint8_t g_ee_fallback[EE_SIZE];
static uint64_t g_busy_until;
static long g_write_us = -1;
static long g_wear_warn;
static uint32_t g_writes[EE_SIZE];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void ee_init(void) {
  const char *path = emu_env("EMU_EEPROM");
  if (!path) path = "emu-eeprom.bin";
  g_write_us = emu_rt_long("EMU_EEPROM_WRITE_US", 3400);
  g_wear_warn = emu_env_long("EMU_EEPROM_WEAR_WARN", 1000);
  int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    emu_log("EEPROM: cannot open %s (%s); using a volatile EEPROM", path, strerror(errno));
    memset(g_ee_fallback, 0xFF, EE_SIZE);
    g_ee = g_ee_fallback;
    return;
  }
  struct stat st;
  fstat(fd, &st);
  if (st.st_size < EE_SIZE) {
    uint8_t ff[EE_SIZE];
    memset(ff, 0xFF, sizeof ff);
    ssize_t w = pwrite(fd, ff + st.st_size, EE_SIZE - st.st_size, st.st_size);
    if (w != EE_SIZE - st.st_size) emu_log("EEPROM: cannot initialise %s", path);
    if (st.st_size == 0) emu_log("EEPROM: created %s (4096 x 0xFF)", path);
  }
  void *p = mmap(NULL, EE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    emu_log("EEPROM: mmap failed (%s); using a volatile EEPROM", strerror(errno));
    memset(g_ee_fallback, 0xFF, EE_SIZE);
    g_ee = g_ee_fallback;
    return;
  }
  g_ee = (uint8_t *)p;
}

static inline unsigned ee_addr(const void *addr) {
  uintptr_t a = (uintptr_t)addr;
  if (a >= EE_SIZE) {
    static int warned;
    if (warned++ < 10) emu_log("EEPROM: address %lu out of range, wraps to %lu (12-bit EEAR)",
                               (unsigned long)a, (unsigned long)(a & (EE_SIZE - 1)));
  }
  return (unsigned)(a & (EE_SIZE - 1));
}

static inline void ee_wait(void) {
  pthread_once(&g_once, ee_init);
  if (g_write_us > 0) {
    uint64_t now = emu_mono_ns();
    if (now < g_busy_until) emu_sleep_until_ns(g_busy_until);
  }
}

extern "C" uint8_t eeprom_read_byte(const uint8_t *addr) {
  ee_wait();
  return g_ee[ee_addr(addr)];
}
extern "C" void eeprom_write_byte(uint8_t *addr, uint8_t value) {
  ee_wait();
  unsigned a = ee_addr(addr);
  g_ee[a] = value;
  if (g_write_us > 0) g_busy_until = emu_mono_ns() + (uint64_t)g_write_us * 1000ull;
  if (g_wear_warn > 0 && ++g_writes[a] == (uint32_t)g_wear_warn)
    emu_log("EEPROM: cell %u written %ld times in this boot (wear)", a, g_wear_warn);
}
extern "C" void eeprom_update_byte(uint8_t *addr, uint8_t value) {
  if (eeprom_read_byte(addr) != value) eeprom_write_byte(addr, value);
}
extern "C" int eeprom_is_ready(void) {
  pthread_once(&g_once, ee_init);
  return g_write_us <= 0 || emu_mono_ns() >= g_busy_until;
}
extern "C" void eeprom_read_block(void *dst, const void *src, size_t n) {
  for (size_t i = 0; i < n; i++) ((uint8_t *)dst)[i] = eeprom_read_byte((const uint8_t *)src + i);
}
extern "C" void eeprom_write_block(const void *src, void *dst, size_t n) {
  for (size_t i = 0; i < n; i++) eeprom_write_byte((uint8_t *)dst + i, ((const uint8_t *)src)[i]);
}
extern "C" void eeprom_update_block(const void *src, void *dst, size_t n) {
  for (size_t i = 0; i < n; i++) eeprom_update_byte((uint8_t *)dst + i, ((const uint8_t *)src)[i]);
}
extern "C" uint16_t eeprom_read_word(const uint16_t *a) { uint16_t v; eeprom_read_block(&v, a, 2); return v; }
extern "C" uint32_t eeprom_read_dword(const uint32_t *a) { uint32_t v; eeprom_read_block(&v, a, 4); return v; }
extern "C" float eeprom_read_float(const float *a) { float v; eeprom_read_block(&v, a, 4); return v; }
extern "C" void eeprom_write_word(uint16_t *a, uint16_t v) { eeprom_write_block(&v, a, 2); }
extern "C" void eeprom_write_dword(uint32_t *a, uint32_t v) { eeprom_write_block(&v, a, 4); }
extern "C" void eeprom_write_float(float *a, float v) { eeprom_write_block(&v, a, 4); }
extern "C" void eeprom_update_word(uint16_t *a, uint16_t v) { eeprom_update_block(&v, a, 2); }
extern "C" void eeprom_update_dword(uint32_t *a, uint32_t v) { eeprom_update_block(&v, a, 4); }
extern "C" void eeprom_update_float(float *a, float v) { eeprom_update_block(&v, a, 4); }
