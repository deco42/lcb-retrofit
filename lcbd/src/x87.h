// x87 extended-precision helpers for the angle computation
#ifndef LCBD_X87_H
#define LCBD_X87_H

#include <float.h>

// Verified on x86-64 only
#if !defined(__x86_64__)
#error "x87 requires an x86 target"
#endif
#if !defined(__GNUC__)
#error "GNU inline assembly required"
#endif
#if LDBL_MANT_DIG != 64
#error "long double must be the x87 80-bit format"
#endif

typedef long double ext; // 80-bit x87 extended precision on x86-64

#define X87_PI ((ext)3.14159265358979323846)
#define X87_PI_2 ((ext)1.57079632679489661923)
#define X87_PI_F ((ext)3.14159265358979323846f)

// fld1 and fptan push one value for a moment: the st(1) clobber keeps that register free.
static inline ext x87_atan(double x)
{
    ext r;
    __asm__("fld1\n\tfpatan" : "=t"(r) : "0"((ext)x) : "st(1)");
    return r;
}

// Only for |x| < 2^63 (fptan leaves larger arguments unchanged)
static inline ext x87_tan(double x)
{
    ext r;
    __asm__("fptan\n\tfstp %%st(0)" : "=t"(r) : "0"((ext)x) : "st(1)");
    return r;
}

static inline ext x87_sq(ext v) { return v * v; }

// Sets extended precision and round to nearest (the Linux default 0x037F, bits 8-11 = 0x3)
static inline unsigned short x87_set_extended_nearest(void)
{
    unsigned short cw, want;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    want = (unsigned short)((cw & ~0x0F00u) | 0x0300u);
    if (want != cw)
        __asm__ volatile("fldcw %0" : : "m"(want));
    return cw;
}

#endif
