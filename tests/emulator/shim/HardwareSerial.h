/* HardwareSerial.h - HOST shim of the AVR HardwareSerial API.
 *
 * Serial  -> stdout (TX) / EMU_SERIAL_IN or stdin (RX).
 * Serial1..3 -> discarded (no input).
 *
 * With EMU_SERIAL_PACE=1 (default in EMU_REALTIME mode) TX is paced at the
 * baud rate with the same 64-byte TX ring as the Mega core (write() blocks
 * when the ring is full: ~87 us per byte at 115200), and RX bytes arrive one
 * byte-time apart into a 64-byte RX ring that DROPS bytes on overflow, like
 * the real UART ISR. With pacing off TX is immediate and RX never drops.
 */
#ifndef HardwareSerial_h
#define HardwareSerial_h

#include <inttypes.h>
#include "Stream.h"

#define SERIAL_TX_BUFFER_SIZE 64
#define SERIAL_RX_BUFFER_SIZE 64

#define SERIAL_5N1 0x00
#define SERIAL_6N1 0x02
#define SERIAL_7N1 0x04
#define SERIAL_8N1 0x06
#define SERIAL_5N2 0x08
#define SERIAL_6N2 0x0A
#define SERIAL_7N2 0x0C
#define SERIAL_8N2 0x0E
#define SERIAL_5E1 0x20
#define SERIAL_6E1 0x22
#define SERIAL_7E1 0x24
#define SERIAL_8E1 0x26
#define SERIAL_5E2 0x28
#define SERIAL_6E2 0x2A
#define SERIAL_7E2 0x2C
#define SERIAL_8E2 0x2E
#define SERIAL_5O1 0x30
#define SERIAL_6O1 0x32
#define SERIAL_7O1 0x34
#define SERIAL_8O1 0x36
#define SERIAL_5O2 0x38
#define SERIAL_6O2 0x3A
#define SERIAL_7O2 0x3C
#define SERIAL_8O2 0x3E

class HardwareSerial : public Stream
{
  public:
    explicit HardwareSerial(int index) : _index(index), _baud(0), _began(false) {}
    void begin(unsigned long baud) { begin(baud, SERIAL_8N1); }
    void begin(unsigned long baud, uint8_t config);
    void end();
    virtual int available(void);
    virtual int peek(void);
    virtual int read(void);
    virtual int availableForWrite(void);
    virtual void flush(void);
    virtual size_t write(uint8_t);
    inline size_t write(unsigned long n) { return write((uint8_t)n); }
    inline size_t write(long n) { return write((uint8_t)n); }
    inline size_t write(unsigned int n) { return write((uint8_t)n); }
    inline size_t write(int n) { return write((uint8_t)n); }
    using Print::write; // pull in write(str) and write(buf, size) from Print
    operator bool() { return true; }

    /* emulator internals */
    int _index;
    unsigned long _baud;
    bool _began;
};

extern HardwareSerial Serial;
extern HardwareSerial Serial1;
extern HardwareSerial Serial2;
extern HardwareSerial Serial3;
#define HAVE_HWSERIAL0
#define HAVE_HWSERIAL1
#define HAVE_HWSERIAL2
#define HAVE_HWSERIAL3

extern void serialEventRun(void) __attribute__((weak));

#endif
