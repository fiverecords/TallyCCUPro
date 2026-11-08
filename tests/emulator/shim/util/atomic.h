#ifndef EMU_UTIL_ATOMIC_H
#define EMU_UTIL_ATOMIC_H
#define ATOMIC_BLOCK(type) for (int __emu_once = 1; __emu_once; __emu_once = 0)
#define ATOMIC_RESTORESTATE
#define ATOMIC_FORCEON
#define NONATOMIC_BLOCK(type) ATOMIC_BLOCK(type)
#define NONATOMIC_RESTORESTATE
#define NONATOMIC_FORCEOFF
#endif
