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

#define X87_PRECISION_ROUNDING_MASK 0x0F00u // control word bits 8-11
#define X87_EXTENDED_NEAREST 0x0300u        // 64-bit precision, round to nearest

// fld1 and fptan push one value for a moment, the st(1) clobber keeps that register free
static inline ext x87_atan(double x)
{
    ext result;
    __asm__("fld1\n\tfpatan" : "=t"(result) : "0"((ext)x) : "st(1)");

    return result;
}

// Only for |x| < 2^63 (fptan leaves larger arguments unchanged)
static inline ext x87_tan(double x)
{
    ext result;
    __asm__("fptan\n\tfstp %%st(0)" : "=t"(result) : "0"((ext)x) : "st(1)");

    return result;
}

static inline ext x87_sq(ext value) { return value * value; }

static inline unsigned short x87_set_extended_nearest(void)
{
    unsigned short cw, want;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    want = (unsigned short)((cw & ~X87_PRECISION_ROUNDING_MASK) | X87_EXTENDED_NEAREST);
    __asm__ volatile("fldcw %0" : : "m"(want));

    return cw;
}

#endif
