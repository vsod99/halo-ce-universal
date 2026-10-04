/*
FLOAT.H

nxdk's C library's <float.h>, which calls the evaluation method
indeterminable (-1). The Xbox build computes floats with SSE and doubles on
the x87 with its precision set to 53 bits at start-up (port/xbox/src), so
every result is rounded to its type as on the other ports (SSE2): method 0,
which the maths (port/third_party/musl-math) is written for.
*/

#ifndef __HALO_XBOX_FLOAT_H
#define __HALO_XBOX_FLOAT_H

#include_next <float.h>

#undef FLT_EVAL_METHOD
#define FLT_EVAL_METHOD 0

#endif /* __HALO_XBOX_FLOAT_H */
