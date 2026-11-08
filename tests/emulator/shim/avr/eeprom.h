/* avr/eeprom.h - HOST shim: 4096-byte EEPROM backed by a file (EMU_EEPROM).
 * Write timing (≈3.4 ms per byte, busy-wait before the next access) is
 * emulated when EMU_EEPROM_WRITE_US > 0 (default in EMU_REALTIME mode).
 * Addresses wrap at 4 KB like the 12-bit EEAR of the ATmega2560. */
#ifndef EMU_AVR_EEPROM_H
#define EMU_AVR_EEPROM_H
#include <stdint.h>
#include <stddef.h>
#include <avr/io.h>

#ifndef EEMEM
#define EEMEM
#endif

#ifdef __cplusplus
extern "C" {
#endif
uint8_t  eeprom_read_byte(const uint8_t *addr);
uint16_t eeprom_read_word(const uint16_t *addr);
uint32_t eeprom_read_dword(const uint32_t *addr);
float    eeprom_read_float(const float *addr);
void     eeprom_read_block(void *dst, const void *src, size_t n);
void     eeprom_write_byte(uint8_t *addr, uint8_t value);
void     eeprom_write_word(uint16_t *addr, uint16_t value);
void     eeprom_write_dword(uint32_t *addr, uint32_t value);
void     eeprom_write_float(float *addr, float value);
void     eeprom_write_block(const void *src, void *dst, size_t n);
void     eeprom_update_byte(uint8_t *addr, uint8_t value);
void     eeprom_update_word(uint16_t *addr, uint16_t value);
void     eeprom_update_dword(uint32_t *addr, uint32_t value);
void     eeprom_update_float(float *addr, float value);
void     eeprom_update_block(const void *src, void *dst, size_t n);
int      eeprom_is_ready(void);
#ifdef __cplusplus
}
#endif
#define eeprom_busy_wait() do {} while (!eeprom_is_ready())

#endif
