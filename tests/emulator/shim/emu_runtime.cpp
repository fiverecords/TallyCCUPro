/* emu_runtime.cpp - main(), time base, pins, simulated watchdog, reset.
 *
 * The sketch runs on the main thread exactly like the AVR core main():
 *   setup(); for (;;) { loop(); serialEventRun(); }
 * Helper threads: watchdog monitor, serial RX reader, W5500 chip.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <atomic>
#include "emu_internal.h"

extern char **environ;

/* ------------------------------------------------------------------ */
/* AVR "registers" and heap symbols used by the firmware's freeRam()   */
/* ------------------------------------------------------------------ */
extern "C" {
volatile uint8_t MCUSR = 0;
volatile uint8_t WDTCSR = 0;
volatile uint8_t SREG = 0x80;
}
/* freeRam() does (int)&v - (int)__brkval. __brkval is set in main() so
 * that the result is about EMU_FREE_RAM bytes minus the host stack depth
 * below main(): a plausible, stable number (the real value cannot be
 * reproduced on the host). */
int __heap_start;
int *__brkval;

/* ------------------------------------------------------------------ */
/* environment                                                         */
/* ------------------------------------------------------------------ */
extern "C" const char *emu_env(const char *name) {
  const char *v = getenv(name);
  if (!v || !*v) return NULL;
  return v;
}
extern "C" long emu_env_long(const char *name, long def) {
  const char *v = emu_env(name);
  if (!v) return def;
  char *end;
  long r = strtol(v, &end, 0);
  if (end == v) return def;
  return r;
}
extern "C" int emu_env_bool(const char *name, int def) {
  const char *v = emu_env(name);
  if (!v) return def;
  if (!strcasecmp(v, "0") || !strcasecmp(v, "no") || !strcasecmp(v, "false") || !strcasecmp(v, "off")) return 0;
  return 1;
}
extern "C" int emu_realtime(void) {
  static int rt = -1;
  if (rt < 0) rt = emu_env_bool("EMU_REALTIME", 1);
  return rt;
}
extern "C" long emu_rt_long(const char *name, long rt_def) {
  return emu_env_long(name, emu_realtime() ? rt_def : 0);
}

/* ------------------------------------------------------------------ */
/* time                                                                */
/* ------------------------------------------------------------------ */
static uint64_t g_t0_ns;
static uint64_t g_millis_offset_ms;

extern "C" uint64_t emu_mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
extern "C" uint64_t emu_mono_us(void) { return emu_mono_ns() / 1000ull; }
extern "C" uint64_t emu_mono_ms(void) { return emu_mono_ns() / 1000000ull; }
extern "C" uint64_t emu_uptime_ms(void) { return (emu_mono_ns() - g_t0_ns) / 1000000ull; }

extern "C" void emu_sleep_until_ns(uint64_t abs_ns) {
  struct timespec ts;
  ts.tv_sec = (time_t)(abs_ns / 1000000000ull);
  ts.tv_nsec = (long)(abs_ns % 1000000000ull);
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}
extern "C" void emu_sleep_ns(uint64_t ns) {
  if (!ns) return;
  emu_sleep_until_ns(emu_mono_ns() + ns);
}
extern "C" uint32_t emu_millis32(void) {
  return (uint32_t)((emu_mono_ns() - g_t0_ns) / 1000000ull + g_millis_offset_ms);
}

extern "C" unsigned long millis(void) { return (unsigned long)emu_millis32(); }
extern "C" unsigned long micros(void) {
  return (unsigned long)(uint32_t)((emu_mono_ns() - g_t0_ns) / 1000ull + g_millis_offset_ms * 1000ull);
}

extern "C" void yield(void) __attribute__((weak));
extern "C" void yield(void) {}

/* Same structure as the AVR core: wait in 1 ms steps calling yield(). */
extern "C" void delay(unsigned long ms) {
  uint64_t end = emu_mono_ns() + (uint64_t)ms * 1000000ull;
  for (;;) {
    yield();
    uint64_t now = emu_mono_ns();
    if (now >= end) break;
    uint64_t left = end - now;
    emu_sleep_ns(left > 1000000ull ? 1000000ull : left);
  }
}
extern "C" void delayMicroseconds(unsigned int us) {
  if (us == 0) return;
  uint64_t end = emu_mono_ns() + (uint64_t)us * 1000ull;
  if (us > 200) { emu_sleep_until_ns(end); return; }
  while (emu_mono_ns() < end) { /* busy wait, like the AVR */ }
}

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */
static int g_boot_count;
static const char *g_reset_cause = "power-on";

extern "C" int emu_boot_count(void) { return g_boot_count; }
extern "C" const char *emu_reset_cause(void) { return g_reset_cause; }

extern "C" void emu_vlog(const char *fmt, va_list ap) {
  char buf[1024];
  int n = snprintf(buf, sizeof buf, "[EMU] ");
  int m = vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap);
  if (m < 0) m = 0;
  n += m;
  if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
  buf[n++] = '\n';
  ssize_t r = write(2, buf, n);
  (void)r;
}
extern "C" void emu_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  emu_vlog(fmt, ap);
  va_end(ap);
}

extern "C" int emu_open_log(const char *envname) {
  const char *p = emu_env(envname);
  if (!p) return -1;
  if (!strcmp(p, "stderr")) return 2;
  if (!strcmp(p, "stdout")) return 1;
  int fd = open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) emu_log("cannot open %s=%s: %s", envname, p, strerror(errno));
  return fd;
}
extern "C" void emu_write_line(int fd, const char *line, size_t len) {
  if (fd < 0) return;
  ssize_t r = write(fd, line, len);
  (void)r;
}

/* ------------------------------------------------------------------ */
/* pins                                                                */
/* ------------------------------------------------------------------ */
uint8_t emu_pin_mode[EMU_NUM_PINS];
uint8_t emu_pin_level[EMU_NUM_PINS];
extern "C" void emu_pin_changed(uint8_t pin);   /* emu_spi.cpp */

extern "C" int emu_eth_cs_pin(void) {
  static int pin = -2;
  if (pin == -2) pin = (int)emu_env_long("EMU_ETH_CS_PIN", 10);
  return pin;
}

extern "C" void pinMode(uint8_t pin, uint8_t mode) {
  if (pin >= EMU_NUM_PINS) return;
  /* AVR: INPUT clears PORTx (no pull-up), INPUT_PULLUP sets it,
   * OUTPUT keeps the last digitalWrite() level. */
  if (mode == 0 /*INPUT*/) { emu_pin_mode[pin] = 0; emu_pin_level[pin] = 0; }
  else if (mode == 2 /*INPUT_PULLUP*/) { emu_pin_mode[pin] = 0; emu_pin_level[pin] = 1; }
  else emu_pin_mode[pin] = 1;
  emu_pin_changed(pin);
}
extern "C" void digitalWrite(uint8_t pin, uint8_t val) {
  if (pin >= EMU_NUM_PINS) return;
  emu_pin_level[pin] = val ? 1 : 0;
  emu_pin_changed(pin);
}
extern "C" int digitalRead(uint8_t pin) {
  if (pin >= EMU_NUM_PINS) return 0;
  return emu_pin_level[pin];
}

static uint32_t g_rng = 0x12345678u;
static uint32_t rng_next(void) {
  g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
  return g_rng;
}
/* Floating analog input: noisy value around mid-scale (used as random seed). */
extern "C" int analogRead(uint8_t pin) {
  (void)pin;
  return (int)(480 + rng_next() % 64);
}
extern "C" void analogReference(uint8_t mode) { (void)mode; }
extern "C" void analogWrite(uint8_t pin, int val) { (void)pin; (void)val; }
extern "C" unsigned long pulseIn(uint8_t, uint8_t, unsigned long) { return 0; }
extern "C" unsigned long pulseInLong(uint8_t, uint8_t, unsigned long) { return 0; }
extern "C" void shiftOut(uint8_t, uint8_t, uint8_t, uint8_t) {}
extern "C" uint8_t shiftIn(uint8_t, uint8_t, uint8_t) { return 0; }
extern "C" void attachInterrupt(uint8_t, void (*)(void), int) {}
extern "C" void detachInterrupt(uint8_t) {}
void tone(uint8_t, unsigned int, unsigned long) {}
void noTone(uint8_t) {}
extern "C" void init(void) {}
extern "C" void initVariant(void) __attribute__((weak));
extern "C" void initVariant(void) {}

/* ------------------------------------------------------------------ */
/* avr-libc stdlib extensions                                          */
/* ------------------------------------------------------------------ */
static char *emu_utoa_any(unsigned long long v, char *s, int radix) {
  if (radix < 2 || radix > 36) { *s = 0; return s; }
  char tmp[72];
  int n = 0;
  do { int d = (int)(v % radix); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= radix; } while (v);
  for (int i = 0; i < n; i++) s[i] = tmp[n - 1 - i];
  s[n] = 0;
  return s;
}
extern "C" char *itoa(int val, char *s, int radix) {
  if (radix == 10 && val < 0) { s[0] = '-'; emu_utoa_any((unsigned long long)(-(long long)val), s + 1, 10); return s; }
  return emu_utoa_any((unsigned int)val, s, radix);
}
extern "C" char *ltoa(long val, char *s, int radix) {
  if (radix == 10 && val < 0) { s[0] = '-'; emu_utoa_any((unsigned long long)(-(long long)val), s + 1, 10); return s; }
  return emu_utoa_any((unsigned long)val, s, radix);
}
extern "C" char *utoa(unsigned int val, char *s, int radix) { return emu_utoa_any(val, s, radix); }
extern "C" char *ultoa(unsigned long val, char *s, int radix) { return emu_utoa_any(val, s, radix); }
extern "C" char *dtostrf(double val, signed char width, unsigned char prec, char *s) {
  sprintf(s, "%*.*f", (int)width, (int)prec, val);
  return s;
}
extern "C" char *dtostre(double val, char *s, unsigned char prec, unsigned char flags) {
  (void)flags;
  sprintf(s, "%.*e", (int)prec, val);
  return s;
}

/* AVR int/long widths for the libc number parsers (see Arduino.h). */
extern "C" int emu_avr_atoi(const char *s) {
  /* avr-libc atoi: no overflow detection, 16-bit accumulation. */
  while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
  int neg = 0;
  if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
  uint16_t acc = 0;
  while (*s >= '0' && *s <= '9') { acc = (uint16_t)(acc * 10u + (uint16_t)(*s - '0')); s++; }
  if (neg) acc = (uint16_t)(0u - acc);
  return (int)(int16_t)acc;
}
extern "C" long emu_avr_atol(const char *s) {
  while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
  int neg = 0;
  if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
  uint32_t acc = 0;
  while (*s >= '0' && *s <= '9') { acc = acc * 10u + (uint32_t)(*s - '0'); s++; }
  if (neg) acc = 0u - acc;
  return (long)(int32_t)acc;
}
extern "C" long emu_avr_strtol(const char *s, char **end, int base) {
  errno = 0;
  long long v = strtoll(s, end, base);
  if (v > 2147483647LL) { errno = ERANGE; return 2147483647L; }
  if (v < -2147483648LL) { errno = ERANGE; return -2147483647L - 1; }
  return (long)v;
}
extern "C" unsigned long emu_avr_strtoul(const char *s, char **end, int base) {
  const char *p = s;
  while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
  errno = 0;
  if (*p == '-') {
    long long v = strtoll(s, end, base);
    if (errno == ERANGE || v < -4294967295LL) { errno = ERANGE; return 0xFFFFFFFFul; }
    return (unsigned long)(uint32_t)(0u - (uint32_t)(-v));
  }
  unsigned long long v = strtoull(s, end, base);
  if (v > 0xFFFFFFFFull) { errno = ERANGE; return 0xFFFFFFFFul; }
  return (unsigned long)v;
}

/* ------------------------------------------------------------------ */
/* watchdog                                                            */
/* ------------------------------------------------------------------ */
static std::atomic<int> g_wdt_enabled(0);
static std::atomic<uint32_t> g_wdt_period_ms(8000);
static std::atomic<uint64_t> g_wdt_last_ns(0);
static int g_wdt_verbose;

static const uint32_t kWdtPeriods[10] = {15, 30, 60, 120, 250, 500, 1000, 2000, 4000, 8000};

/* WDTCSR bits mirrored like avr-libc: wdt_enable() writes WDIE = 0 */
extern "C" void emu_wdt_enable(uint8_t value) {
  if (value > 9) value = 9;
  WDTCSR = (uint8_t)((WDTCSR & ~0x40) | 0x08);
  g_wdt_period_ms.store(kWdtPeriods[value]);
  g_wdt_last_ns.store(emu_mono_ns());   /* avr-libc wdt_enable() does "wdr" first */
  g_wdt_enabled.store(1);
  if (g_wdt_verbose) emu_log("wdt_enable(%u ms) at t=%u", kWdtPeriods[value], emu_millis32());
}
extern "C" void emu_wdt_disable(void) {
  WDTCSR = (uint8_t)(WDTCSR & ~0x48);
  g_wdt_enabled.store(0);
  if (g_wdt_verbose) emu_log("wdt_disable() at t=%u", emu_millis32());
}
extern "C" void emu_wdt_reset(void) { g_wdt_last_ns.store(emu_mono_ns()); }

/* Firmware's watchdog ISR (interrupt + reset mode), if it has one */
extern "C" void WDT_vect(void) __attribute__((weak));
/* SIGUSR1: reset pin (RAM kept, MCUSR EXTRF with EMU_MCUSR_KEEP) */
static std::atomic<int> g_ext_reset(0);
static void on_sigusr1(int) { g_ext_reset.store(1); }

static void *wdt_thread(void *) {
  prctl(PR_SET_NAME, "emu-wdt", 0, 0, 0);
  int disabled = emu_env_bool("EMU_WDT_DISABLE", 0);
  long exit_after = emu_env_long("EMU_EXIT_AFTER_MS", 0);
  for (;;) {
    emu_sleep_ns(1000000ull);
    uint64_t now = emu_mono_ns();
    if (exit_after > 0 && (now - g_t0_ns) / 1000000ull >= (uint64_t)exit_after) {
      emu_serial_flush_out();
      emu_log("EMU_EXIT_AFTER_MS=%ld reached, exiting", exit_after);
      _exit(0);
    }
    if (g_ext_reset.load()) emu_do_reset("external", 0);
    if (!g_wdt_enabled.load() || disabled) continue;
    uint64_t last = g_wdt_last_ns.load();
    uint64_t el = now > last ? now - last : 0;
    if (el > (uint64_t)g_wdt_period_ms.load() * 1000000ull) {
      /* Interrupt + reset mode: the first timeout runs the ISR (hardware
       * clears WDIE); the reset follows at the next timeout */
      if ((WDTCSR & 0x40) && WDT_vect) {
        WDTCSR = (uint8_t)(WDTCSR & ~0x40);
        emu_log("WDT interrupt after %llu ms", (unsigned long long)(el / 1000000ull));
        g_wdt_last_ns.store(emu_mono_ns());
        WDT_vect();
        continue;
      }
      emu_do_reset("watchdog", el / 1000000ull);
    }
  }
  return NULL;
}

/* ------------------------------------------------------------------ */
/* reset                                                               */
/* ------------------------------------------------------------------ */
static char **g_argv;
static emu_reset_hook_t g_hooks[16];
static int g_nhooks;
static std::atomic<int> g_resetting(0);

extern "C" void emu_add_reset_hook(emu_reset_hook_t hook) {
  if (g_nhooks < 16) g_hooks[g_nhooks++] = hook;
}

extern "C" void emu_do_reset(const char *why, uint64_t elapsed_ms) {
  if (g_resetting.exchange(1)) { for (;;) pause(); }
  emu_serial_flush_out();
  if (!strcmp(why, "watchdog")) {
    char line[96];
    int n = snprintf(line, sizeof line, "[EMU] WATCHDOG RESET after %llu ms\n", (unsigned long long)elapsed_ms);
    ssize_t r = write(2, line, n);
    (void)r;
  } else {
    emu_log("RESET (%s)", why);
  }
  for (int i = 0; i < g_nhooks; i++) g_hooks[i]();

  if (emu_env_bool("EMU_WDT_EXIT", 0)) _exit(42);
  long max_boots = emu_env_long("EMU_MAX_BOOTS", 0);
  if (max_boots > 0 && g_boot_count + 1 >= max_boots) {
    emu_log("EMU_MAX_BOOTS=%ld reached, exiting with code 43", max_boots);
    _exit(43);
  }

  static char boot_var[64], cause_var[64], inherit_var[1024], noinit_var[4200];
  snprintf(boot_var, sizeof boot_var, "EMU_BOOT_COUNT=%d", g_boot_count + 1);
  snprintf(cause_var, sizeof cause_var, "EMU_RESET_CAUSE=%s", why);
  inherit_var[0] = 0;
  emu_w5500_prepare_exec(inherit_var, sizeof inherit_var);
  emu_noinit_env(noinit_var, sizeof noinit_var);

  int n = 0;
  for (char **e = environ; *e; e++) n++;
  char **env = (char **)calloc(n + 5, sizeof(char *));
  int k = 0;
  for (char **e = environ; *e; e++) {
    if (!strncmp(*e, "EMU_BOOT_COUNT=", 15) || !strncmp(*e, "EMU_RESET_CAUSE=", 16) ||
        !strncmp(*e, "EMU_W5500_INHERIT=", 18) || !strncmp(*e, "EMU_NOINIT=", 11)) continue;
    env[k++] = *e;
  }
  env[k++] = boot_var;
  env[k++] = cause_var;
  if (noinit_var[0]) env[k++] = noinit_var;
  if (inherit_var[0]) env[k++] = inherit_var;
  env[k] = NULL;
  execve("/proc/self/exe", g_argv, env);
  emu_log("execve failed: %s", strerror(errno));
  _exit(43);
}

/* ------------------------------------------------------------------ */
/* startup                                                             */
/* ------------------------------------------------------------------ */
__attribute__((constructor(101))) static void emu_early_init(void) {
  g_t0_ns = emu_mono_ns();
  g_millis_offset_ms = (uint64_t)emu_env_long("EMU_MILLIS_OFFSET", 0);
  g_boot_count = (int)emu_env_long("EMU_BOOT_COUNT", 0);
  const char *cause = emu_env("EMU_RESET_CAUSE");
  if (cause && !strcmp(cause, "watchdog")) g_reset_cause = "watchdog";
  if (cause && !strcmp(cause, "external")) g_reset_cause = "external";
  if (emu_env_bool("EMU_MCUSR_KEEP", 0)) {
    MCUSR = (uint8_t)(g_reset_cause[0] == 'w' ? (1 << 3) /*WDRF*/ :
                      g_reset_cause[0] == 'e' ? (1 << 1) /*EXTRF*/ : (1 << 0) /*PORF*/);
  } else {
    MCUSR = 0;  /* Mega stk500v2 bootloader clears MCUSR before the sketch */
  }
  const char *seed = emu_env("EMU_SEED");
  g_rng = seed ? (uint32_t)strtoul(seed, NULL, 0) * 2654435761u + 1 : (uint32_t)(g_t0_ns ^ (getpid() << 16));
  if (!g_rng) g_rng = 1;
  g_wdt_verbose = emu_env_bool("EMU_VERBOSE", 0);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGUSR1, on_sigusr1);
  prctl(PR_SET_TIMERSLACK, 1000, 0, 0, 0);  /* 1 us: precise short sleeps */
}

extern "C" void setup(void);
extern "C" void loop(void);
void serialEventRun(void) __attribute__((weak));

int main(int argc, char **argv) {
  (void)argc;
  g_argv = argv;
  emu_log("boot #%d (cause: %s) pid %d, realtime=%d, bind=%s, sd=%s, eeprom=%s",
          g_boot_count, g_reset_cause, (int)getpid(), emu_realtime(),
          emu_env("EMU_BIND_IP") ? emu_env("EMU_BIND_IP") : "127.0.0.1",
          emu_env("EMU_SD_DIR") ? emu_env("EMU_SD_DIR") : "(none: SD begin() fails)",
          emu_env("EMU_EEPROM") ? emu_env("EMU_EEPROM") : "emu-eeprom.bin");
  pthread_t th;
  pthread_create(&th, NULL, wdt_thread, NULL);
  pthread_detach(th);

  char marker;
  long free_ram = emu_env_long("EMU_FREE_RAM", 2000);
  __brkval = (int *)(void *)(&marker - free_ram);

  init();
  initVariant();
  setup();
  for (;;) {
    loop();
    if (serialEventRun) serialEventRun();
  }
  return 0;
}
