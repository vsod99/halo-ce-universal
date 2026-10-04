/*
NSPIRE_FLOAT_COMPARE.S

Single-precision comparisons for the soft-float build, in place of libgcc's
(_arm_cmpsf2.o, every one of whose symbols is defined here, so the linker
takes none of it). The game compares floats all over (collision, physics,
the renderer's tests), and libgcc does each one as three calls, a push and
a pop of five registers (__aeabi_fcmplt calls __aeabi_cfcmpeq, which calls
__cmpsf2): about 5% of a frame. Here each is a dozen instructions in
registers.

A float's bits order like a signed integer once the negative ones are
mapped to 0x80000000 - bits (sign and magnitude to two's complement; -0
and +0 both become 0). A NaN (the bits less the sign above 0x7F800000) is
unordered: every ordered test of it is false, as in libgcc:

- __aeabi_fcmp{eq,lt,le,gt,ge}: 1 or 0;
- __cmpsf2, __eqsf2, __nesf2, __ltsf2, __lesf2: negative, zero or positive,
  1 when unordered; __gtsf2, __gesf2: -1 when unordered;
- __aeabi_cfcmple, __aeabi_cfcmpeq (a against b) and __aeabi_cfrcmple (b
  against a): the flags only, Z set when equal and C clear when less (C set
  and Z clear when unordered), every register but ip kept.
*/

	.syntax unified
	.arm
	.text

/* r0 and r1 (a and b) to ordered integers, or to label when either is a
NaN; uses r2 and r3 */
.macro keys unordered
	mov r2, r0, lsl #1
	cmp r2, #0xFF000000
	bhi \unordered
	mov r3, r1, lsl #1
	cmp r3, #0xFF000000
	bhi \unordered
	cmp r0, #0
	rsblt r0, r0, #0x80000000
	cmp r1, #0
	rsblt r1, r1, #0x80000000
	cmp r0, r1
.endm

.macro function name
	.global \name
	.type \name, %function
	.balign 4
\name:
.endm

function __aeabi_fcmpeq
	keys 1f
	moveq r0, #1
	movne r0, #0
	bx lr
1:	mov r0, #0
	bx lr
	.size __aeabi_fcmpeq, . - __aeabi_fcmpeq

function __aeabi_fcmplt
	keys 1f
	movlt r0, #1
	movge r0, #0
	bx lr
1:	mov r0, #0
	bx lr
	.size __aeabi_fcmplt, . - __aeabi_fcmplt

function __aeabi_fcmple
	keys 1f
	movle r0, #1
	movgt r0, #0
	bx lr
1:	mov r0, #0
	bx lr
	.size __aeabi_fcmple, . - __aeabi_fcmple

function __aeabi_fcmpgt
	keys 1f
	movgt r0, #1
	movle r0, #0
	bx lr
1:	mov r0, #0
	bx lr
	.size __aeabi_fcmpgt, . - __aeabi_fcmpgt

function __aeabi_fcmpge
	keys 1f
	movge r0, #1
	movlt r0, #0
	bx lr
1:	mov r0, #0
	bx lr
	.size __aeabi_fcmpge, . - __aeabi_fcmpge

/* -1, 0 or 1; 1 when unordered */
	.global __eqsf2, __nesf2, __ltsf2, __lesf2
	.set __eqsf2, __cmpsf2
	.set __nesf2, __cmpsf2
	.set __ltsf2, __cmpsf2
	.set __lesf2, __cmpsf2
function __cmpsf2
	keys 1f
	mvnlt r0, #0
	moveq r0, #0
	movgt r0, #1
	bx lr
1:	mov r0, #1
	bx lr
	.size __cmpsf2, . - __cmpsf2

/* -1, 0 or 1; -1 when unordered */
	.global __gtsf2
	.set __gtsf2, __gesf2
function __gesf2
	keys 1f
	mvnlt r0, #0
	moveq r0, #0
	movgt r0, #1
	bx lr
1:	mvn r0, #0
	bx lr
	.size __gesf2, . - __gesf2

/* the flags alone, every register but ip kept: the integers compared
unsigned with their sign bits flipped, so that C is clear when a < b */
	.global __aeabi_cfcmpeq
	.set __aeabi_cfcmpeq, __aeabi_cfcmple
function __aeabi_cfcmple
	push {r0, r1}
	mov ip, r0, lsl #1
	cmp ip, #0xFF000000
	bhi 1f
	mov ip, r1, lsl #1
	cmp ip, #0xFF000000
	bhi 1f
	cmp r0, #0
	rsblt r0, r0, #0x80000000
	cmp r1, #0
	rsblt r1, r1, #0x80000000
	eor r0, r0, #0x80000000
	eor r1, r1, #0x80000000
	cmp r0, r1
	/* (unordered: the bhi left C set and Z clear) */
1:	pop {r0, r1}
	bx lr
	.size __aeabi_cfcmple, . - __aeabi_cfcmple

function __aeabi_cfrcmple
	push {r0, r1}
	mov ip, r0, lsl #1
	cmp ip, #0xFF000000
	bhi 1f
	mov ip, r1, lsl #1
	cmp ip, #0xFF000000
	bhi 1f
	cmp r0, #0
	rsblt r0, r0, #0x80000000
	cmp r1, #0
	rsblt r1, r1, #0x80000000
	eor r0, r0, #0x80000000
	eor r1, r1, #0x80000000
	cmp r1, r0
1:	pop {r0, r1}
	bx lr
	.size __aeabi_cfrcmple, . - __aeabi_cfrcmple
