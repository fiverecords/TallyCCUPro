/* emu_serial.cpp - HardwareSerial on the host.
 *
 * TX: Serial -> stdout (write(2) per byte, so nothing is lost on reset or
 *     crash). With EMU_SERIAL_PACE (default on in EMU_REALTIME mode)
 *     write() blocks like the AVR core once the 64-byte TX ring is full:
 *     one byte every 10/baud seconds.
 * RX: EMU_SERIAL_IN=<fifo|file|none>, otherwise stdin. A reader thread
 *     (started by Serial.begin()) feeds a 64-byte ring; with pacing the bytes
 *     arrive one byte-time apart and are DROPPED when the ring is full (like
 *     the UART ISR); without pacing the reader waits instead.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <atomic>
#include "emu_internal.h"
#include "HardwareSerial.h"

HardwareSerial Serial(0);
HardwareSerial Serial1(1);
HardwareSerial Serial2(2);
HardwareSerial Serial3(3);

static int g_pace = -1;
static uint64_t g_byte_ns;           /* 0 = not begun / no pacing */
static uint64_t g_tx_free_at;        /* virtual time the UART is idle again */

/* RX ring: single producer (reader thread), single consumer (sketch). */
static uint8_t g_rx[SERIAL_RX_BUFFER_SIZE];
static std::atomic<unsigned> g_rx_head(0), g_rx_tail(0);
static std::atomic<unsigned long> g_rx_dropped(0);
static std::atomic<int> g_reader_started(0);

static int pace_enabled(void) {
  if (g_pace < 0) g_pace = (int)emu_rt_long("EMU_SERIAL_PACE", 1);
  return g_pace;
}

static void *reader_thread(void *) {
  prctl(PR_SET_NAME, "emu-serial", 0, 0, 0);
  const char *path = emu_env("EMU_SERIAL_IN");
  int fd = 0;
  int tail_mode = 0;
  if (path && !strcmp(path, "none")) return NULL;
  if (path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISFIFO(st.st_mode)) {
      fd = open(path, O_RDWR | O_CLOEXEC);  /* RDWR: writers may come and go */
    } else {
      fd = open(path, O_RDONLY | O_CLOEXEC);
      tail_mode = 1;
    }
    if (fd < 0) { emu_log("cannot open EMU_SERIAL_IN=%s: %s", path, strerror(errno)); return NULL; }
  } else {
    struct stat st;
    if (fstat(0, &st) == 0 && S_ISREG(st.st_mode)) tail_mode = 1;
  }
  uint64_t next_ns = 0;
  uint8_t buf[256];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      emu_log("serial input read error: %s", strerror(errno));
      return NULL;
    }
    if (n == 0) {
      if (!tail_mode) return NULL;  /* EOF on a pipe/tty: no more input */
      emu_sleep_ns(50000000ull);    /* regular file: follow it like tail -f */
      continue;
    }
    for (ssize_t i = 0; i < n; i++) {
      int pace = pace_enabled() && g_byte_ns;
      if (pace) {
        uint64_t now = emu_mono_ns();
        if (next_ns < now) next_ns = now;
        emu_sleep_until_ns(next_ns);
        next_ns += g_byte_ns;
      }
      for (;;) {
        unsigned head = g_rx_head.load(), tail = g_rx_tail.load();
        unsigned nh = (head + 1) % SERIAL_RX_BUFFER_SIZE;
        if (nh != tail) {
          g_rx[head] = buf[i];
          g_rx_head.store(nh);
          break;
        }
        if (pace) {  /* ring full: the real ISR drops the byte */
          unsigned long d = ++g_rx_dropped;
          if (d == 1 || d % 1000 == 0) emu_log("Serial RX overflow: %lu byte(s) dropped (64-byte ring full)", d);
          break;
        }
        emu_sleep_ns(200000ull);
      }
    }
  }
  return NULL;
}

void HardwareSerial::begin(unsigned long baud, uint8_t config) {
  (void)config;
  _baud = baud;
  _began = true;
  if (_index != 0) return;
  g_byte_ns = baud ? (uint64_t)(10ull * 1000000000ull / baud) : 0;  /* 8N1 = 10 bits */
  g_tx_free_at = 0;
  if (!g_reader_started.exchange(1)) {
    pthread_t th;
    pthread_create(&th, NULL, reader_thread, NULL);
    pthread_detach(th);
  }
}

void HardwareSerial::end() { _began = false; }

int HardwareSerial::available(void) {
  if (_index != 0) return 0;
  unsigned head = g_rx_head.load(), tail = g_rx_tail.load();
  return (int)((SERIAL_RX_BUFFER_SIZE + head - tail) % SERIAL_RX_BUFFER_SIZE);
}

int HardwareSerial::peek(void) {
  if (_index != 0) return -1;
  unsigned head = g_rx_head.load(), tail = g_rx_tail.load();
  if (head == tail) return -1;
  return g_rx[tail];
}

int HardwareSerial::read(void) {
  if (_index != 0) return -1;
  unsigned head = g_rx_head.load(), tail = g_rx_tail.load();
  if (head == tail) return -1;
  uint8_t c = g_rx[tail];
  g_rx_tail.store((tail + 1) % SERIAL_RX_BUFFER_SIZE);
  return c;
}

int HardwareSerial::availableForWrite(void) {
  if (_index != 0 || !pace_enabled() || !g_byte_ns) return SERIAL_TX_BUFFER_SIZE - 1;
  uint64_t now = emu_mono_ns();
  if (g_tx_free_at <= now) return SERIAL_TX_BUFFER_SIZE - 1;
  uint64_t queued = (g_tx_free_at - now) / g_byte_ns;
  if (queued >= SERIAL_TX_BUFFER_SIZE - 1) return 0;
  return (int)(SERIAL_TX_BUFFER_SIZE - 1 - queued);
}

void HardwareSerial::flush(void) {
  if (_index != 0 || !pace_enabled() || !g_byte_ns) return;
  uint64_t now = emu_mono_ns();
  if (g_tx_free_at > now) emu_sleep_until_ns(g_tx_free_at);
}

size_t HardwareSerial::write(uint8_t c) {
  if (_index != 0) return 1;
  if (pace_enabled() && g_byte_ns) {
    uint64_t now = emu_mono_ns();
    if (g_tx_free_at < now) g_tx_free_at = now;
    const uint64_t cap = (uint64_t)SERIAL_TX_BUFFER_SIZE * g_byte_ns;
    if (g_tx_free_at > now + cap) emu_sleep_until_ns(g_tx_free_at - cap);
    g_tx_free_at += g_byte_ns;
  }
  ssize_t r = ::write(1, &c, 1);
  (void)r;
  return 1;
}

extern "C" void emu_serial_flush_out(void) { /* stdout is written unbuffered */ }
