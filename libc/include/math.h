#ifndef _MATH_H
#define _MATH_H 1

#include <sys/cdefs.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* x87 FPU implementations for freestanding kernel */

static inline double sin(double x) {
    double result;
    __asm__ volatile("fldl %1; fsin; fstpl %0" : "=m"(result) : "m"(x));
    return result;
}

static inline double cos(double x) {
    double result;
    __asm__ volatile("fldl %1; fcos; fstpl %0" : "=m"(result) : "m"(x));
    return result;
}

static inline double sqrt(double x) {
    double result;
    __asm__ volatile("fldl %1; fsqrt; fstpl %0" : "=m"(result) : "m"(x));
    return result;
}

static inline double fabs(double x) {
    double result;
    __asm__ volatile("fldl %1; fabs; fstpl %0" : "=m"(result) : "m"(x));
    return result;
}

static inline double pow(double base, double exp) {
    /* x^y = 2^(y * log2(x)) */
    if (base == 0.0) return 0.0;
    double result;
    __asm__ volatile(
        "fldl %2       \n\t"   /* ST(0) = exp */
        "fldl %1       \n\t"   /* ST(0) = base, ST(1) = exp */
        "fyl2x         \n\t"   /* ST(0) = exp * log2(base) */
        "fld %%st(0)   \n\t"   /* dup */
        "frndint       \n\t"   /* ST(0) = int part */
        "fxch %%st(1)  \n\t"
        "fsub %%st(1), %%st(0) \n\t"  /* ST(0) = frac part */
        "f2xm1         \n\t"   /* ST(0) = 2^frac - 1 */
        "fld1          \n\t"
        "faddp         \n\t"   /* ST(0) = 2^frac */
        "fscale        \n\t"   /* ST(0) = 2^frac * 2^int = 2^(exp*log2(base)) */
        "fstp %%st(1)  \n\t"   /* clean stack */
        "fstpl %0      \n\t"
        : "=m"(result)
        : "m"(base), "m"(exp)
    );
    return result;
}

static inline double floor(double x) {
    double result = (double)(long long)x;
    if (result > x) result -= 1.0;
    return result;
}

static inline double ceil(double x) {
    double result = (double)(long long)x;
    if (result < x) result += 1.0;
    return result;
}

static inline float sinf(float x) { return (float)sin((double)x); }
static inline float cosf(float x) { return (float)cos((double)x); }
static inline float sqrtf(float x) { return (float)sqrt((double)x); }
static inline float fabsf(float x) { return (float)fabs((double)x); }
static inline float powf(float b, float e) { return (float)pow((double)b, (double)e); }

#ifdef __cplusplus
}
#endif

#endif /* _MATH_H */
