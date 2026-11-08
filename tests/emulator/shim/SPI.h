/* SPI.h - HOST shim of the AVR SPI library API.
 * Bytes are delivered to the emulated W5500 while its chip-select pin
 * (EMU_ETH_CS_PIN, default 10) is an OUTPUT driven LOW; otherwise MISO reads
 * 0xFF. The SPI clock of the Mega (8 MHz max) plus AVR overhead is emulated
 * with EMU_SPI_BYTE_NS / EMU_SPI_FRAME_NS (defaults 1200 / 2000 ns in
 * EMU_REALTIME mode). SPI_HAS_TRANSFER_BUF is NOT defined (as on AVR). */
#ifndef _SPI_H_INCLUDED
#define _SPI_H_INCLUDED

#include <Arduino.h>

#define SPI_HAS_TRANSACTION 1
#define SPI_HAS_NOTUSINGINTERRUPT 1
#define SPI_ATOMIC_VERSION 1

#define SPI_CLOCK_DIV4 0x00
#define SPI_CLOCK_DIV16 0x01
#define SPI_CLOCK_DIV64 0x02
#define SPI_CLOCK_DIV128 0x03
#define SPI_CLOCK_DIV2 0x04
#define SPI_CLOCK_DIV8 0x05
#define SPI_CLOCK_DIV32 0x06

#define SPI_MODE0 0x00
#define SPI_MODE1 0x04
#define SPI_MODE2 0x08
#define SPI_MODE3 0x0C

class SPISettings {
public:
  SPISettings(uint32_t clock, uint8_t bitOrder, uint8_t dataMode)
    : _clock(clock), _bitOrder(bitOrder), _dataMode(dataMode) {}
  SPISettings() : _clock(4000000), _bitOrder(MSBFIRST), _dataMode(SPI_MODE0) {}
  uint32_t _clock;
  uint8_t _bitOrder;
  uint8_t _dataMode;
};

class SPIClass {
public:
  static void begin();
  static void usingInterrupt(uint8_t interruptNumber) { (void)interruptNumber; }
  static void notUsingInterrupt(uint8_t interruptNumber) { (void)interruptNumber; }
  static void beginTransaction(SPISettings settings) { (void)settings; }
  static uint8_t transfer(uint8_t data);
  static uint16_t transfer16(uint16_t data) {
    uint8_t hi = transfer((uint8_t)(data >> 8));
    uint8_t lo = transfer((uint8_t)(data & 0xFF));
    return (uint16_t)((hi << 8) | lo);
  }
  static void transfer(void *buf, size_t count) {
    uint8_t *p = (uint8_t *)buf;
    for (size_t i = 0; i < count; i++) p[i] = transfer(p[i]);
  }
  static void endTransaction(void) {}
  static void end();
  static void setBitOrder(uint8_t bitOrder) { (void)bitOrder; }
  static void setDataMode(uint8_t dataMode) { (void)dataMode; }
  static void setClockDivider(uint8_t clockDiv) { (void)clockDiv; }
  static void attachInterrupt() {}
  static void detachInterrupt() {}
};

extern SPIClass SPI;

#endif
