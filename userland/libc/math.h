#ifndef _USER_MATH_H
#define _USER_MATH_H

/* Mathematical constants */
#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_E         2.71828182845904523536
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define M_LOG2E     1.44269504088896340736
#define M_LOG10E    0.43429448190325182765
#define M_SQRT2     1.41421356237309504880
#define M_SQRT1_2   0.70710678118654752440
#define INFINITY    __builtin_inf()
#define NAN         __builtin_nan("")
#define HUGE_VAL    __builtin_huge_val()
#define HUGE_VALF   __builtin_huge_valf()

/* Basic math functions (implemented in math.c using x87 FPU) */
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

double sqrt(double x);
double fabs(double x);
double fmod(double x, double y);

double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);

double pow(double base, double exp);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);

double ldexp(double x, int exp);
double frexp(double x, int *exp);

/* Float variants */
float sinf(float x);
float cosf(float x);
float tanf(float x);
float sqrtf(float x);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float roundf(float x);
float truncf(float x);
float powf(float base, float exp);
float expf(float x);
float logf(float x);
float log2f(float x);
float fmodf(float x, float y);
float atan2f(float y, float x);

/* Classification */
int isnan(double x);
int isinf(double x);
int isfinite(double x);

#endif
