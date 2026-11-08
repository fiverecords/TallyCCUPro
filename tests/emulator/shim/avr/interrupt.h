/* avr/interrupt.h - HOST shim: no interrupts on the host. */
#ifndef EMU_AVR_INTERRUPT_H
#define EMU_AVR_INTERRUPT_H
#define sei() do {} while (0)
#define cli() do {} while (0)
#define ISR(vector, ...) extern "C" void vector(void); void vector(void)
#define SIGNAL(vector) ISR(vector)
#define EMPTY_INTERRUPT(vector) ISR(vector) {}
#define ISR_BLOCK
#define ISR_NOBLOCK
#define ISR_NAKED
#define reti()
#endif
