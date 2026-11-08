/* emu_internal.h - interfaces shared by the emulator shim modules.
 * Not included by firmware code. Only C headers here (the firmware-facing
 * Arduino.h defines min/max/abs macros that clash with C++ std headers). */
#ifndef EMU_INTERNAL_H
#define EMU_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- configuration (environment) ---- */
const char *emu_env(const char *name);                 /* NULL if unset/empty */
long        emu_env_long(const char *name, long def);
int         emu_env_bool(const char *name, int def);
/* realtime knob: value of `name` if set, else (EMU_REALTIME ? rt_def : 0) */
long        emu_rt_long(const char *name, long rt_def);
int         emu_realtime(void);

/* ---- time ---- */
uint64_t emu_mono_ns(void);
uint64_t emu_mono_us(void);
uint64_t emu_mono_ms(void);
uint64_t emu_uptime_ms(void);          /* ms since this boot (process start) */
void     emu_sleep_ns(uint64_t ns);
void     emu_sleep_until_ns(uint64_t abs_mono_ns);
uint32_t emu_millis32(void);           /* what millis() returns */

/* ---- logging ---- */
void emu_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));  /* "[EMU] ..." on stderr */
void emu_vlog(const char *fmt, va_list ap);
int  emu_boot_count(void);
const char *emu_reset_cause(void);     /* "power-on" or "watchdog" */

/* JSON-lines log helpers. Returns fd (or -1). */
int  emu_open_log(const char *envname);  /* opens path in env (append), "stderr" -> 2, unset -> -1 */
void emu_write_line(int fd, const char *line, size_t len); /* single write(2) */

/* ---- pins ---- */
#define EMU_NUM_PINS 70
extern uint8_t emu_pin_mode[EMU_NUM_PINS];
extern uint8_t emu_pin_level[EMU_NUM_PINS];
int  emu_eth_cs_pin(void);

/* ---- SPI <-> W5500 ---- */
void    emu_w5500_cs(int selected);        /* chip-select edge */
uint8_t emu_w5500_spi(uint8_t mosi);       /* one byte on the bus */
void    emu_w5500_prepare_exec(char *envbuf, size_t envbuf_size); /* "EMU_W5500_INHERIT=..." */
void    emu_spi_pace_frame(void);

/* ---- reset ---- */
/* Called by the watchdog: never returns (execv or _exit). */
void emu_do_reset(const char *why, uint64_t elapsed_ms) __attribute__((noreturn));
/* Modules register hooks run before a reset (e.g. SD unsynced-file rollback). */
typedef void (*emu_reset_hook_t)(void);
void emu_add_reset_hook(emu_reset_hook_t hook);

/* ---- .noinit across resets (emu_noinit.cpp) ---- */
void emu_noinit_env(char *buf, size_t n);   /* "EMU_NOINIT=<hex>" or "" */

/* ---- misc ---- */
void emu_serial_flush_out(void);

#ifdef __cplusplus
}
#endif

#endif
