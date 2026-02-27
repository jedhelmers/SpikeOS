/*
 * math.c — minimal math library using x87 FPU instructions.
 *
 * The i386 target has a hardware FPU (x87), so we use inline asm
 * for transcendental functions rather than soft-float.
 */

#include "math.h"

/* ------------------------------------------------------------------ */
/*  Trigonometric functions                                            */
/* ------------------------------------------------------------------ */

double sin(double x) {
    double result;
    __asm__ volatile("fldl %1; fsin; fstpl %0" : "=m"(result) : "m"(x));
    return result;
}

double cos(double x) {
    double result;
    __asm__ volatile("fldl %1; fcos; fstpl %0" : "=m"(result) : "m"(x));
    return result;
}

double tan(double x) {
    double result;
    __asm__ volatile(
        "fldl %1; fptan; fstp %%st(0); fstpl %0"
        : "=m"(result) : "m"(x)
    );
    return result;
}

double atan(double x) {
    double result;
    __asm__ volatile(
        "fldl %1; fld1; fpatan; fstpl %0"
        : "=m"(result) : "m"(x)
    );
    return result;
}

double atan2(double y, double x) {
    double result;
    __asm__ volatile(
        "fldl %1; fldl %2; fpatan; fstpl %0"
        : "=m"(result) : "m"(y), "m"(x)
    );
    return result;
}

double asin(double x) {
    /* asin(x) = atan2(x, sqrt(1 - x*x)) */
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x) {
    /* acos(x) = atan2(sqrt(1 - x*x), x) */
    return atan2(sqrt(1.0 - x * x), x);
}

/* ------------------------------------------------------------------ */
/*  Exponential / logarithmic functions                                */
/* ------------------------------------------------------------------ */

double sqrt(double x) {
    double result;
    __asm__ volatile("fldl %1; fsqrt; fstpl %0" : "=m"(result) : "m"(x));
    return result;
}

double log2(double x) {
    double result;
    __asm__ volatile(
        "fld1; fldl %1; fyl2x; fstpl %0"
        : "=m"(result) : "m"(x)
    );
    return result;
}

double log(double x) {
    /* ln(x) = log2(x) / log2(e) = log2(x) * ln(2) */
    return log2(x) * M_LN2;
}

double log10(double x) {
    /* log10(x) = log2(x) / log2(10) = log2(x) * log10(2) */
    return log2(x) * 0.30102999566398119521;
}

double exp(double x) {
    /* exp(x) = 2^(x * log2(e)) */
    double result;
    __asm__ volatile(
        "fldl %1          \n\t"  /* ST(0) = x */
        "fldl2e            \n\t"  /* ST(0) = log2(e), ST(1) = x */
        "fmulp             \n\t"  /* ST(0) = x * log2(e) */
        "fld %%st(0)       \n\t"  /* duplicate */
        "frndint            \n\t"  /* ST(0) = int part, ST(1) = x*log2e */
        "fsub %%st(0), %%st(1) \n\t" /* ST(1) = frac part */
        "fxch              \n\t"  /* ST(0) = frac, ST(1) = int */
        "f2xm1             \n\t"  /* ST(0) = 2^frac - 1 */
        "fld1              \n\t"
        "faddp             \n\t"  /* ST(0) = 2^frac */
        "fscale            \n\t"  /* ST(0) = 2^frac * 2^int = e^x */
        "fstp %%st(1)      \n\t"  /* clean up */
        "fstpl %0          \n\t"
        : "=m"(result) : "m"(x)
    );
    return result;
}

double pow(double base, double exponent) {
    /* Handle special cases */
    if (exponent == 0.0) return 1.0;
    if (base == 0.0) return 0.0;
    if (base == 1.0) return 1.0;

    /* Check if exponent is an integer for negative bases */
    if (base < 0.0) {
        int iexp = (int)exponent;
        if ((double)iexp == exponent) {
            double result = exp(exponent * log(-base));
            return (iexp & 1) ? -result : result;
        }
        return NAN;  /* negative base with non-integer exponent */
    }

    /* pow(base, exp) = exp(exp * ln(base)) */
    return exp(exponent * log(base));
}

double ldexp(double x, int e) {
    /* ldexp(x, exp) = x * 2^exp */
    double result;
    double dexp = (double)e;
    __asm__ volatile(
        "fldl %2           \n\t"
        "fldl %1           \n\t"
        "fscale            \n\t"
        "fstp %%st(1)      \n\t"
        "fstpl %0          \n\t"
        : "=m"(result) : "m"(x), "m"(dexp)
    );
    return result;
}

double frexp(double x, int *e) {
    if (x == 0.0) {
        *e = 0;
        return 0.0;
    }
    /* Use log2 to extract exponent */
    double l = log2(fabs(x));
    int exp_val = (int)floor(l) + 1;
    *e = exp_val;
    return x / ldexp(1.0, exp_val);
}

/* ------------------------------------------------------------------ */
/*  Absolute value / modulo                                            */
/* ------------------------------------------------------------------ */

double fabs(double x) {
    double result;
    __asm__ volatile("fldl %1; fabs; fstpl %0" : "=m"(result) : "m"(x));
    return result;
}

double fmod(double x, double y) {
    if (y == 0.0) return NAN;
    double result;
    __asm__ volatile(
        "fldl %2           \n\t"  /* ST(0) = y */
        "fldl %1           \n\t"  /* ST(0) = x, ST(1) = y */
        "1: fprem           \n\t"
        "fnstsw %%ax        \n\t"
        "testw $0x0400, %%ax\n\t"
        "jnz 1b             \n\t"
        "fstp %%st(1)       \n\t"
        "fstpl %0           \n\t"
        : "=m"(result) : "m"(x), "m"(y) : "ax"
    );
    return result;
}

/* ------------------------------------------------------------------ */
/*  Rounding functions (pure C — avoids FPU CW inline asm issues)      */
/* ------------------------------------------------------------------ */

double trunc(double x) {
    /* Cast to long long, then back — truncates toward zero.
       Works for values within long long range (~9.2e18). */
    if (x != x) return x;  /* NaN */
    if (x >= 9.2e18 || x <= -9.2e18) return x;  /* already integral */
    return (double)(long long)x;
}

double floor(double x) {
    double t = trunc(x);
    if (x < 0.0 && t != x) return t - 1.0;
    return t;
}

double ceil(double x) {
    double t = trunc(x);
    if (x > 0.0 && t != x) return t + 1.0;
    return t;
}

double round(double x) {
    return (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
}

/* ------------------------------------------------------------------ */
/*  Float variants (cast through double — x87 is 80-bit internally)   */
/* ------------------------------------------------------------------ */

float sinf(float x)  { return (float)sin((double)x); }
float cosf(float x)  { return (float)cos((double)x); }
float tanf(float x)  { return (float)tan((double)x); }
float sqrtf(float x) { return (float)sqrt((double)x); }
float fabsf(float x) { return (float)fabs((double)x); }
float floorf(float x) { return (float)floor((double)x); }
float ceilf(float x)  { return (float)ceil((double)x); }
float roundf(float x) { return (float)round((double)x); }
float truncf(float x) { return (float)trunc((double)x); }
float powf(float base, float e) { return (float)pow((double)base, (double)e); }
float expf(float x)   { return (float)exp((double)x); }
float logf(float x)   { return (float)log((double)x); }
float log2f(float x)  { return (float)log2((double)x); }
float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }
float atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }

/* ------------------------------------------------------------------ */
/*  Classification                                                     */
/* ------------------------------------------------------------------ */

int isnan(double x)    { return __builtin_isnan(x); }
int isinf(double x)    { return __builtin_isinf(x); }
int isfinite(double x) { return __builtin_isfinite(x); }
