#if !defined(INCLUDE_CPU_H)
# define INCLUDE_CPU_H
/*
 * This is based on code by David Hedley, from pcemu.
 * Most of the CPU emulation was rewritten and code was extended to support
 * 80186 and 80286 instructions.
 */

# include <stdint.h>

// CPU-generation differences (instruction availability, PUSH SP, shift-count
// handling, and divide-error behaviour) are now run-time selected in cpu.c.

enum
{
    AX = 0,
    CX,
    DX,
    BX,
    SP,
    BP,
    SI,
    DI
};

enum
{
    ES = 0,
    CS,
    SS,
    DS,
    NoSeg
};

# define SetZFB(x) (ZF = !(uint8_t)(x))
# define SetZFW(x) (ZF = !(uint16_t)(x))
# define SetPF(x)  (PF = parity_table[(uint8_t)(x)])
# define SetSFW(x) (SF = (x)&0x8000)
# define SetSFB(x) (SF = (x)&0x80)

# define CompressFlags()                                                                 \
    (uint16_t)(CF | 2 | (PF << 2) | (!(!AF) << 4) | (ZF << 6) | (!(!SF) << 7) |          \
               (TF << 8) | (IF << 9) | (DF << 10) | (!(!OF) << 11))

# define ExpandFlags(f)                                                                  \
    {                                                                                    \
        CF = (f)&1;                                                                      \
        PF = ((f)&4) == 4;                                                               \
        AF = (f)&16;                                                                     \
        ZF = ((f)&64) == 64;                                                             \
        SF = (f)&128;                                                                    \
        TF = ((f)&256) == 256;                                                           \
        IF = ((f)&512) == 512;                                                           \
        DF = ((f)&1024) == 1024;                                                         \
        OF = (f)&2048;                                                                   \
    }
#endif
