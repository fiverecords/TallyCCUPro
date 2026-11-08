#ifndef EMU_UTIL_DELAY_H
#define EMU_UTIL_DELAY_H
#ifdef __cplusplus
extern "C" {
#endif
void delayMicroseconds(unsigned int us);
void delay(unsigned long ms);
#ifdef __cplusplus
}
#endif
#define _delay_us(us) delayMicroseconds((unsigned int)(us))
#define _delay_ms(ms) delay((unsigned long)(ms))
#endif
