/* avr/wdt.h - HOST shim with a SIMULATED watchdog.
 * A monitor thread checks the time since the last wdt_reset(); when it
 * exceeds the configured period it prints
 *   [EMU] WATCHDOG RESET after <ms> ms
 * on stderr and re-executes the emulator (execv of /proc/self/exe, same argv
 * and env, EMU_BOOT_COUNT+1). With EMU_WDT_EXIT=1 it exits with code 42. */
#ifndef EMU_AVR_WDT_H
#define EMU_AVR_WDT_H
#include <stdint.h>

#define WDTO_15MS   0
#define WDTO_30MS   1
#define WDTO_60MS   2
#define WDTO_120MS  3
#define WDTO_250MS  4
#define WDTO_500MS  5
#define WDTO_1S     6
#define WDTO_2S     7
#define WDTO_4S     8
#define WDTO_8S     9

#ifdef __cplusplus
extern "C" {
#endif
void emu_wdt_enable(uint8_t value);
void emu_wdt_disable(void);
void emu_wdt_reset(void);
#ifdef __cplusplus
}
#endif

#define wdt_enable(value) emu_wdt_enable(value)
#define wdt_disable()     emu_wdt_disable()
#define wdt_reset()       emu_wdt_reset()

#endif
