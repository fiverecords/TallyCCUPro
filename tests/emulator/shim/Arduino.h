/*
 * Arduino.h - HOST shim for the TallyCCU Pro emulator (Arduino Mega 2560).
 *
 * Mirrors the public surface of the AVR core 1.8.6 Arduino.h (same macros:
 * min/max/abs/constrain/round are macros, like on AVR) but implements time,
 * pins and serial on the host. The portable core files (Print, Stream,
 * WString, IPAddress, WMath) are compiled unmodified from the AVR core.
 *
 * Differences that cannot be hidden on x86_64: int is 32 bit (AVR: 16),
 * long is 64 bit (AVR: 32), double is 64 bit (AVR: 32), pointers are 64 bit.
 * millis()/micros() are truncated to 32 bits so they wrap like on AVR.
 */
#ifndef Arduino_h
#define Arduino_h

/* All standard headers first: the min/max/abs/round macros below must not
 * be seen by libstdc++ headers (same order as the real core). */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>

#include <avr/pgmspace.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include "binary.h"

#define EMU_HOST 1

#ifdef __cplusplus
extern "C" {
#endif

void yield(void);

#define HIGH 0x1
#define LOW  0x0

#define INPUT 0x0
#define OUTPUT 0x1
#define INPUT_PULLUP 0x2

#define PI 3.1415926535897932384626433832795
#define HALF_PI 1.5707963267948966192313216916398
#define TWO_PI 6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105
#define EULER 2.718281828459045235360287471352

#define SERIAL  0x0
#define DISPLAY 0x1

#define LSBFIRST 0
#define MSBFIRST 1

#define CHANGE 1
#define FALLING 2
#define RISING 3

#define INTERNAL1V1 2
#define INTERNAL2V56 3
#define DEFAULT 1
#define EXTERNAL 0

/* Same macro definitions as AVR core 1.8.6 */
#ifdef abs
#undef abs
#endif
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define abs(x) ((x)>0?(x):-(x))
#define constrain(amt,low,high) ((amt)<(low)?(low):((amt)>(high)?(high):(amt)))
#define round(x)     ((x)>=0?(long)((x)+0.5):(long)((x)-0.5))
#define radians(deg) ((deg)*DEG_TO_RAD)
#define degrees(rad) ((rad)*RAD_TO_DEG)
#define sq(x) ((x)*(x))

#define interrupts() sei()
#define noInterrupts() cli()

#define clockCyclesPerMicrosecond() ( F_CPU / 1000000L )
#define clockCyclesToMicroseconds(a) ( (a) / clockCyclesPerMicrosecond() )
#define microsecondsToClockCycles(a) ( (a) * clockCyclesPerMicrosecond() )

#define lowByte(w) ((uint8_t) ((w) & 0xff))
#define highByte(w) ((uint8_t) ((w) >> 8))

#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define bitToggle(value, bit) ((value) ^= (1UL << (bit)))
#define bitWrite(value, bit, bitvalue) ((bitvalue) ? bitSet(value, bit) : bitClear(value, bit))

#ifndef _NOP
#define _NOP() do { __asm__ volatile ("nop"); } while (0)
#endif

typedef unsigned int word;

#define bit(b) (1UL << (b))

typedef bool boolean;
typedef uint8_t byte;

void init(void);
void initVariant(void);

void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t val);
int digitalRead(uint8_t pin);
int analogRead(uint8_t pin);
void analogReference(uint8_t mode);
void analogWrite(uint8_t pin, int val);

unsigned long millis(void);
unsigned long micros(void);
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);
unsigned long pulseIn(uint8_t pin, uint8_t state, unsigned long timeout);
unsigned long pulseInLong(uint8_t pin, uint8_t state, unsigned long timeout);

void shiftOut(uint8_t dataPin, uint8_t clockPin, uint8_t bitOrder, uint8_t val);
uint8_t shiftIn(uint8_t dataPin, uint8_t clockPin, uint8_t bitOrder);

void attachInterrupt(uint8_t interruptNum, void (*userFunc)(void), int mode);
void detachInterrupt(uint8_t interruptNum);

void setup(void);
void loop(void);

/* avr-libc stdlib extensions (not in glibc) */
char *itoa(int val, char *s, int radix);
char *ltoa(long val, char *s, int radix);
char *utoa(unsigned int val, char *s, int radix);
char *ultoa(unsigned long val, char *s, int radix);
char *dtostrf(double val, signed char width, unsigned char prec, char *s);
char *dtostre(double val, char *s, unsigned char prec, unsigned char flags);

/* AVR widths for libc number parsers: on the Mega int is 16 bit and long is
 * 32 bit, so atoi("65537") == 1 and strtol() saturates at +-2^31. These
 * wrappers reproduce that (avr-libc atoi/atol accumulate without overflow
 * checks). Build with -DEMU_NO_AVR_INT to get the host libc behaviour. */
int emu_avr_atoi(const char *s);
long emu_avr_atol(const char *s);
long emu_avr_strtol(const char *s, char **end, int base);
unsigned long emu_avr_strtoul(const char *s, char **end, int base);

/* avr-libc string/memory copies are simple forward byte loops, so code that
 * (illegally) passes overlapping buffers, e.g. strncpy(buf, buf, n), behaves
 * deterministically on the Mega. glibc's vectorised versions do not (they can
 * drop bytes), so the AVR semantics are reproduced here. */
static inline char *emu_avr_strncpy(char *d, const char *s, size_t n) {
  size_t i = 0;
  for (; i < n && s[i]; i++) d[i] = s[i];
  for (; i < n; i++) d[i] = 0;
  return d;
}
static inline char *emu_avr_strcpy(char *d, const char *s) {
  size_t i = 0;
  do { d[i] = s[i]; } while (s[i++]);
  return d;
}
static inline void *emu_avr_memcpy(void *d, const void *s, size_t n) {
  unsigned char *dd = (unsigned char *)d; const unsigned char *ss = (const unsigned char *)s;
  for (size_t i = 0; i < n; i++) dd[i] = ss[i];
  return d;
}
static inline char *emu_avr_strcat(char *d, const char *s) { emu_avr_strcpy(d + strlen(d), s); return d; }
static inline char *emu_avr_strncat(char *d, const char *s, size_t n) {
  char *p = d + strlen(d); size_t i = 0;
  for (; i < n && s[i]; i++) p[i] = s[i];
  p[i] = 0;
  return d;
}
#ifndef EMU_NO_AVR_STRING
#define strncpy emu_avr_strncpy
#define strcpy emu_avr_strcpy
#define memcpy emu_avr_memcpy
#define strcat emu_avr_strcat
#define strncat emu_avr_strncat
#endif
#ifndef EMU_NO_AVR_INT
#define atoi(s) emu_avr_atoi(s)
#define atol(s) emu_avr_atol(s)
#define strtol(s, e, b) emu_avr_strtol((s), (e), (b))
#define strtoul(s, e, b) emu_avr_strtoul((s), (e), (b))
#endif

#define NOT_A_PIN 0
#define NOT_A_PORT 0
#define NOT_AN_INTERRUPT -1
#define NOT_ON_TIMER 0

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ---- pins_arduino.h (variant "mega") ---- */
#define NUM_DIGITAL_PINS            70
#define NUM_ANALOG_INPUTS           16
#define analogInputToDigitalPin(p)  ((p < 16) ? (p) + 54 : -1)
#define digitalPinHasPWM(p)         (((p) >= 2 && (p) <= 13) || ((p) >= 44 && (p)<= 46))
#define digitalPinToInterrupt(p) ((p) == 2 ? 0 : ((p) == 3 ? 1 : ((p) >= 18 && (p) <= 21 ? 23 - (p) : NOT_AN_INTERRUPT)))
/* PIN_SPI_SS is intentionally NOT defined: Ethernet's w5100.cpp would pick
 * it as default CS on non-AVR builds (the real AVR build uses pin 10). */
static const uint8_t SS   = 53;
static const uint8_t MOSI = 51;
static const uint8_t MISO = 50;
static const uint8_t SCK  = 52;
#define PIN_WIRE_SDA        (20)
#define PIN_WIRE_SCL        (21)
static const uint8_t SDA = PIN_WIRE_SDA;
static const uint8_t SCL = PIN_WIRE_SCL;
#define LED_BUILTIN 13
#define PIN_A0   (54)
static const uint8_t A0 = 54;  static const uint8_t A1 = 55;
static const uint8_t A2 = 56;  static const uint8_t A3 = 57;
static const uint8_t A4 = 58;  static const uint8_t A5 = 59;
static const uint8_t A6 = 60;  static const uint8_t A7 = 61;
static const uint8_t A8 = 62;  static const uint8_t A9 = 63;
static const uint8_t A10 = 64; static const uint8_t A11 = 65;
static const uint8_t A12 = 66; static const uint8_t A13 = 67;
static const uint8_t A14 = 68; static const uint8_t A15 = 69;

#ifdef __cplusplus
#include "WCharacter.h"
#include "WString.h"
#include "HardwareSerial.h"

unsigned int makeWord(unsigned int w);
unsigned int makeWord(unsigned char h, unsigned char l);

#define word(...) makeWord(__VA_ARGS__)

unsigned long pulseIn(uint8_t pin, uint8_t state, unsigned long timeout = 1000000L);
unsigned long pulseInLong(uint8_t pin, uint8_t state, unsigned long timeout = 1000000L);

void tone(uint8_t _pin, unsigned int frequency, unsigned long duration = 0);
void noTone(uint8_t _pin);

/* WMath.cpp (compiled unmodified from the AVR core) */
long random(long);
long random(long, long);
void randomSeed(unsigned long);
long map(long, long, long, long, long);
#endif /* __cplusplus */

#endif /* Arduino_h */
