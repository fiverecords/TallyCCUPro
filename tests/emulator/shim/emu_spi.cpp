/* emu_spi.cpp - SPI bus: chip-select tracking and SPI timing. */
#include <stdio.h>
#include <stdint.h>
#include "emu_internal.h"
#include "SPI.h"

SPIClass SPI;

static int g_eth_sel;
static long g_byte_ns = -1, g_frame_ns;
static uint64_t g_spi_target;     /* virtual time the bus is free again */
static unsigned long g_contention;

static inline void spi_pace(uint64_t ns) {
  if (!ns) return;
  uint64_t now = emu_mono_ns();
  if (g_spi_target < now) g_spi_target = now;
  g_spi_target += ns;
  /* sleep in chunks >= 50 us to keep the average rate exact and cheap */
  if (g_spi_target > now + 50000ull) emu_sleep_until_ns(g_spi_target);
}

static void spi_cfg(void) {
  if (g_byte_ns >= 0) return;
  g_byte_ns = emu_rt_long("EMU_SPI_BYTE_NS", 1200);
  g_frame_ns = emu_rt_long("EMU_SPI_FRAME_NS", 2000);
}

extern "C" void emu_spi_pace_frame(void) { spi_cfg(); spi_pace((uint64_t)g_frame_ns); }

extern "C" void emu_pin_changed(uint8_t pin) {
  if ((int)pin != emu_eth_cs_pin()) return;
  int sel = emu_pin_mode[pin] == 1 && emu_pin_level[pin] == 0;
  if (sel == g_eth_sel) return;
  g_eth_sel = sel;
  emu_w5500_cs(sel);
  if (!sel) emu_spi_pace_frame();
}

void SPIClass::begin() {
  /* AVR SPI.begin(): SS (53) becomes an OUTPUT driven HIGH */
  if (!emu_pin_mode[53]) { emu_pin_level[53] = 1; emu_pin_mode[53] = 1; }
}
void SPIClass::end() {}

uint8_t SPIClass::transfer(uint8_t data) {
  spi_cfg();
  spi_pace((uint64_t)g_byte_ns);
  if (!g_eth_sel) return 0xFF;
  static int sdcs = -2;
  if (sdcs == -2) sdcs = (int)emu_env_long("EMU_SD_CS_PIN", 4);
  if (sdcs >= 0 && sdcs < EMU_NUM_PINS && emu_pin_mode[sdcs] == 1 && emu_pin_level[sdcs] == 0) {
    if (g_contention++ % 1000 == 0)
      emu_log("SPI bus contention: SD CS (pin %d) is LOW during a W5500 transfer (%lu bytes so far)", sdcs, g_contention);
  }
  return emu_w5500_spi(data);
}
