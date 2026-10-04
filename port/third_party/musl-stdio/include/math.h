/*
MATH.H

The <math.h> the musl files in ../src see: only what they use. long double
is double on the targets that build this (the Microsoft ABI), so musl's
long double helpers are the double ones (fmod, scalbn and frexp are musl's
too, in ../src: exact, whichever C library). Written for this port, not
musl's.
*/

#ifndef __HALO_MUSL_STDIO_MATH_H
#define __HALO_MUSL_STDIO_MATH_H

#include <float.h>

_Static_assert(LDBL_MANT_DIG == 53, "long double is double");

/* no excess precision (the x87 at 53 bits computes the Xbox's doubles) */
typedef double double_t;
typedef float float_t;

#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")

#define isnan(x) __builtin_isnan(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)

double frexp(double x, int *e);
float frexpf(float x, int *e);
double scalbn(double x, int n);
float scalbnf(float x, int n);
double fmod(double x, double y);
float fmodf(float x, float y);

#define frexpl frexp
#define scalbnl scalbn
#define fmodl fmod
#define copysignl(x, y) __builtin_copysign(x, y)
#define fabsl(x) __builtin_fabs(x)

#endif /* __HALO_MUSL_STDIO_MATH_H */
