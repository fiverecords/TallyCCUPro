/* avr/pgmspace.h - HOST shim: flash and RAM are the same address space.
 * NOTE: code that dereferences a PROGMEM pointer without pgm_read_* works on
 * the host but reads garbage on a real AVR; the emulator cannot detect it. */
#ifndef EMU_AVR_PGMSPACE_H
#define EMU_AVR_PGMSPACE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdarg.h>

#ifndef PROGMEM
#define PROGMEM
#endif
#define PGM_P const char *
#define PGM_VOID_P const void *
#define PSTR(s) (s)
#define PSTRN(s, n) (s)

typedef char prog_char;
typedef unsigned char prog_uchar;
typedef int8_t prog_int8_t;
typedef uint8_t prog_uint8_t;
typedef int16_t prog_int16_t;
typedef uint16_t prog_uint16_t;
typedef int32_t prog_int32_t;
typedef uint32_t prog_uint32_t;

#define pgm_read_byte(addr)        (*(const uint8_t *)(addr))
#define pgm_read_byte_near(addr)   pgm_read_byte(addr)
#define pgm_read_byte_far(addr)    pgm_read_byte(addr)
#define pgm_read_word(addr)        (*(const uint16_t *)(addr))
#define pgm_read_word_near(addr)   pgm_read_word(addr)
#define pgm_read_word_far(addr)    pgm_read_word(addr)
#define pgm_read_dword(addr)       (*(const uint32_t *)(addr))
#define pgm_read_dword_near(addr)  pgm_read_dword(addr)
#define pgm_read_dword_far(addr)   pgm_read_dword(addr)
#define pgm_read_float(addr)       (*(const float *)(addr))
#define pgm_read_float_near(addr)  pgm_read_float(addr)
/* On AVR a pointer is 16 bit; on the host the table holds native pointers. */
#define pgm_read_ptr(addr)         (*(void * const *)(addr))
#define pgm_read_ptr_near(addr)    pgm_read_ptr(addr)
#define pgm_read_ptr_far(addr)     pgm_read_ptr(addr)
#define pgm_get_far_address(var)   ((uintptr_t)(&(var)))

#define memchr_P    memchr
#define memcmp_P    memcmp
#define memccpy_P   memccpy
#define memcpy_P    memcpy
#define memmem_P    memmem
#define memrchr_P   memrchr
#define strcat_P    strcat
#define strchr_P    strchr
#define strchrnul_P strchrnul
#define strcmp_P    strcmp
#define strcpy_P    strcpy
#define strcasecmp_P  strcasecmp
#define strcasestr_P  strcasestr
#define strcspn_P   strcspn
#define strlcat_P   strlcat
#define strlcpy_P   strlcpy
#define strlen_P    strlen
#define strnlen_P   strnlen
#define strncmp_P   strncmp
#define strncasecmp_P strncasecmp
#define strncat_P   strncat
#define strncpy_P   strncpy
#define strpbrk_P   strpbrk
#define strrchr_P   strrchr
#define strsep_P    strsep
#define strspn_P    strspn
#define strstr_P    strstr
#define strtok_P    strtok
#define strtok_r_P  strtok_r
#define memcpy_PF   memcpy
#define strcpy_PF   strcpy
#define strlen_PF   strlen
/* printf family: %S (PROGMEM string on AVR) is not translated; the firmware
 * does not use it. */
#define printf_P    printf
#define sprintf_P   sprintf
#define snprintf_P  snprintf
#define vsprintf_P  vsprintf
#define vsnprintf_P vsnprintf
#define fprintf_P   fprintf
#define fputs_P     fputs
#define puts_P      puts
#define sscanf_P    sscanf

/* strlcpy/strlcat come from glibc >= 2.38 */

#endif /* EMU_AVR_PGMSPACE_H */
