/* avr/io.h - HOST shim (ATmega2560 subset). Registers are plain variables. */
#ifndef EMU_AVR_IO_H
#define EMU_AVR_IO_H
#include <stdint.h>

#ifndef __AVR_ATmega2560__
#define __AVR_ATmega2560__ 1   /* the target; __AVR__ itself is NOT defined */
#endif

#define RAMSTART 0x200
#define RAMEND   0x21FF
#define XRAMEND  0x21FF
#define E2END    0x0FFF
#define E2PAGESIZE 8
#define FLASHEND 0x3FFFF
#define SPM_PAGESIZE 256

/* MCUSR bits */
#define PORF  0
#define EXTRF 1
#define BORF  2
#define WDRF  3
#define JTRF  4
/* WDTCSR bits */
#define WDP0 0
#define WDP1 1
#define WDP2 2
#define WDE  3
#define WDCE 4
#define WDP3 5
#define WDIE 6
#define WDIF 7

#define _BV(bit) (1 << (bit))
#define bit_is_set(sfr, bit) (_SFR_BYTE(sfr) & _BV(bit))
#define bit_is_clear(sfr, bit) (!(_SFR_BYTE(sfr) & _BV(bit)))
#define _SFR_BYTE(sfr) (sfr)

#ifdef __cplusplus
extern "C" {
#endif
/* Reset cause register. At boot the emulator sets it like the Mega's
 * stk500v2 bootloader leaves it: 0 (the bootloader clears it). With
 * EMU_MCUSR_KEEP=1 it holds the real cause (PORF / WDRF). */
extern volatile uint8_t MCUSR;
/* Plain variable: writing it does NOT drive the emulated watchdog
 * (use wdt_enable/wdt_disable, as the firmware does). */
extern volatile uint8_t WDTCSR;
extern volatile uint8_t SREG;
#ifdef __cplusplus
}
#endif
#define MCUCSR MCUSR

#endif
