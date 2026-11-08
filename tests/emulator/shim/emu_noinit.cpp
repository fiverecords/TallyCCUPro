/* emu_noinit.cpp - the firmware's .noinit section survives emulated resets.
 *
 * On the AVR, RAM keeps its content across a watchdog reset, the reset pin
 * and a jump to 0, and holds noise after a power-up. Here a reset is an
 * execve(): the section is passed to the next boot in EMU_NOINIT (hex) and
 * restored before any constructor of the firmware runs. A power-up (no
 * EMU_NOINIT) fills it with pseudo-random bytes (EMU_NOINIT_POWERON=zero
 * leaves zeros). The section is found in the ELF section table of
 * /proc/self/exe. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <elf.h>
#include <link.h>
#include <time.h>
#include "emu_internal.h"

static uint8_t *g_ni;
static size_t g_ni_size;

static int first_phdr(struct dl_phdr_info *info, size_t, void *data) {
  *(uintptr_t *)data = (uintptr_t)info->dlpi_addr;
  return 1;   /* the main program comes first */
}

static void find_noinit(void) {
  int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  Elf64_Ehdr eh;
  if (pread(fd, &eh, sizeof eh, 0) != (ssize_t)sizeof eh || eh.e_shentsize != sizeof(Elf64_Shdr)) { close(fd); return; }
  Elf64_Shdr *sh = (Elf64_Shdr *)calloc(eh.e_shnum, sizeof(Elf64_Shdr));
  if (!sh) { close(fd); return; }
  if (pread(fd, sh, eh.e_shnum * sizeof(Elf64_Shdr), eh.e_shoff) == (ssize_t)(eh.e_shnum * sizeof(Elf64_Shdr)) &&
      eh.e_shstrndx < eh.e_shnum) {
    Elf64_Shdr *ss = &sh[eh.e_shstrndx];
    char *names = (char *)malloc(ss->sh_size + 1);
    if (names && pread(fd, names, ss->sh_size, ss->sh_offset) == (ssize_t)ss->sh_size) {
      names[ss->sh_size] = 0;
      for (int i = 0; i < eh.e_shnum; i++) {
        if (sh[i].sh_name < ss->sh_size && !strcmp(names + sh[i].sh_name, ".noinit")) {
          uintptr_t base = 0;
          dl_iterate_phdr(first_phdr, &base);
          g_ni = (uint8_t *)(base + sh[i].sh_addr);
          g_ni_size = sh[i].sh_size;
          break;
        }
      }
    }
    free(names);
  }
  free(sh);
  close(fd);
}

__attribute__((constructor(102))) static void emu_noinit_init(void) {
  find_noinit();
  if (!g_ni || !g_ni_size) return;
  const char *hex = emu_env("EMU_NOINIT");
  if (hex && strlen(hex) == g_ni_size * 2) {
    for (size_t i = 0; i < g_ni_size; i++) {
      unsigned v;
      sscanf(hex + 2 * i, "%2x", &v);
      g_ni[i] = (uint8_t)v;
    }
    return;
  }
  const char *mode = emu_env("EMU_NOINIT_POWERON");
  if (mode && !strcmp(mode, "zero")) { memset(g_ni, 0, g_ni_size); return; }
  uint32_t x = (uint32_t)emu_env_long("EMU_SEED", 0);
  if (!x) x = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 12);
  for (size_t i = 0; i < g_ni_size; i++) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_ni[i] = (uint8_t)x;
  }
}

/* "EMU_NOINIT=<hex>" for the next boot; empty if there is no section */
extern "C" void emu_noinit_env(char *buf, size_t n) {
  buf[0] = 0;
  if (!g_ni || !g_ni_size || n < 12 + g_ni_size * 2) return;
  int k = snprintf(buf, n, "EMU_NOINIT=");
  for (size_t i = 0; i < g_ni_size; i++) k += snprintf(buf + k, n - k, "%02x", g_ni[i]);
}
