/*
NSPIRE_ABORT.S

The exception handlers of the Nspire port (nspire_exceptions.c decides).

Each handler switches to a stack of its own at once (the OS's exception
stacks may be small; their pointers are kept in nspire_os_abort_sp and
nspire_os_undefined_sp and put back before returning or going on to the
OS), pushes the interrupted code's r0-r12 and the exception's lr as a
frame, and calls nspire_exception(kind, frame, fault address, fault status):

- 1, handled: the faulting instruction runs again (a page the pager
  brought in);
- 2, handled and done: execution goes on after it (an unaligned access
  done byte by byte);
- 3, a crash to report: the handler returns into nspire_crash_trampoline in
  the interrupted mode, which writes the crash to the log and exits;
- 4, a page to read in (nspire_pagein_address): the handler returns into
  nspire_pagein_trampoline in the interrupted mode (the program's own),
  which saves the registers, has nspire_paging_service read the block from
  the map file, and runs the faulting instruction again with every register
  and flag as it was;
- 0, not ours: on to the OS's handler, with every register as it was.

Frame: r0-r12 at 0-48, the exception's lr at 52.
*/

	.syntax unified
	.arm

	.bss
	.balign 8
nspire_exception_stack:
	.space 0x4000
nspire_exception_stack_top:
nspire_crash_stack:
	.space 0x2000
nspire_crash_stack_top:

	.text

	.global nspire_abort_chain, nspire_prefetch_chain, nspire_undefined_chain
	.global nspire_os_abort_sp, nspire_os_undefined_sp
	.balign 4
/* in the code, so that "ldr pc, <label>" and "ldr sp, <label>" reach them
without a free register */
nspire_abort_chain:
	.word 0
nspire_prefetch_chain:
	.word 0
nspire_undefined_chain:
	.word 0
nspire_os_abort_sp:
	.word 0
nspire_os_undefined_sp:
	.word 0

/* The FIQ vector, when sampling (nspire_paging.c): the sampling timer's
interrupt, routed to the FIQ while the program runs (Ndless runs it with
interrupts masked, the FIQ alone let through). Each sample is two words in
nspire_sampling's ring (count, mask, buffer, a saved r0): the interrupted
code's pc + 4 (the FIQ's lr) and its own lr (a leaf function's caller),
read by going into its mode a moment with r0 kept aside. Then the timer's
interrupt is cleared and the code goes on. No stack is used. */
	.global nspire_fiq_entry
	.type nspire_fiq_entry, %function
nspire_fiq_entry:
	ldr r8, =nspire_sampling
	ldr r9, [r8]
	ldr r10, [r8, #4]
	and r10, r9, r10
	add r9, r9, #1
	str r9, [r8]
	ldr r9, [r8, #8]
	add r10, r9, r10, lsl #3
	str lr, [r10]
	/* the interrupted mode's lr (user mode's through system mode) */
	str r0, [r8, #12]
	mrs r9, spsr
	and r9, r9, #0x1F
	cmp r9, #0x10
	moveq r9, #0x1F
	orr r9, r9, #0xC0
	msr cpsr_c, r9
	mov r0, lr
	msr cpsr_c, #0xD1
	str r0, [r10, #4]
	ldr r0, [r8, #12]
	ldr r8, =0x900D000C
	mov r9, #1
	str r9, [r8]
	subs pc, lr, #4
	.ltorg
	.size nspire_fiq_entry, . - nspire_fiq_entry

	.global nspire_abort_entry
	.type nspire_abort_entry, %function
nspire_abort_entry:
	ldr sp, =nspire_exception_stack_top
	stmfd sp!, {r0-r12, lr}
	mov r0, #1
	mov r1, sp
	mrc p15, 0, r2, c6, c0, 0
	mrc p15, 0, r3, c5, c0, 0
	bl nspire_exception
	cmp r0, #1
	beq abort_retry
	cmp r0, #2
	beq abort_skip
	cmp r0, #3
	beq crash_leave_abort
	cmp r0, #4
	beq pagein_leave
	ldmfd sp!, {r0-r12, lr}
	ldr sp, nspire_os_abort_sp
	ldr pc, nspire_abort_chain
abort_retry:
	ldmfd sp!, {r0-r12, lr}
	ldr sp, nspire_os_abort_sp
	subs pc, lr, #8
abort_skip:
	ldmfd sp!, {r0-r12, lr}
	ldr sp, nspire_os_abort_sp
	subs pc, lr, #4
	.size nspire_abort_entry, . - nspire_abort_entry

	.global nspire_prefetch_entry
	.type nspire_prefetch_entry, %function
nspire_prefetch_entry:
	ldr sp, =nspire_exception_stack_top
	stmfd sp!, {r0-r12, lr}
	mov r0, #2
	mov r1, sp
	mrc p15, 0, r2, c6, c0, 0
	mrc p15, 0, r3, c5, c0, 0
	bl nspire_exception
	cmp r0, #3
	beq crash_leave_abort
	ldmfd sp!, {r0-r12, lr}
	ldr sp, nspire_os_abort_sp
	ldr pc, nspire_prefetch_chain
	.size nspire_prefetch_entry, . - nspire_prefetch_entry

	.global nspire_undefined_entry
	.type nspire_undefined_entry, %function
nspire_undefined_entry:
	ldr sp, =nspire_exception_stack_top
	stmfd sp!, {r0-r12, lr}
	mov r0, #3
	mov r1, sp
	mrc p15, 0, r2, c6, c0, 0
	mrc p15, 0, r3, c5, c0, 0
	bl nspire_exception
	cmp r0, #3
	beq crash_leave_undefined
	ldmfd sp!, {r0-r12, lr}
	ldr sp, nspire_os_undefined_sp
	ldr pc, nspire_undefined_chain
	.size nspire_undefined_entry, . - nspire_undefined_entry

/* leave the exception (the OS's stack put back) into a trampoline, in the
interrupted mode, in ARM state */
crash_leave_abort:
	ldmfd sp!, {r0-r12, lr}
	ldr sp, nspire_os_abort_sp
	b crash_leave
crash_leave_undefined:
	ldmfd sp!, {r0-r12, lr}
	ldr sp, nspire_os_undefined_sp
crash_leave:
	mrs lr, spsr
	bic lr, lr, #0x20		/* ARM state */
	msr spsr_cxsf, lr
	ldr lr, =nspire_crash_trampoline
	movs pc, lr

pagein_leave:
	ldmfd sp!, {r0-r12, lr}
	ldr sp, nspire_os_abort_sp
	ldr lr, =nspire_pagein_trampoline
	movs pc, lr

/* in the faulting code's mode, with its registers and stack: saves them
below its stack, reads the page in on an 8-byte aligned stack, and goes
back to the faulting instruction */
nspire_pagein_trampoline:
	sub sp, sp, #8			/* [sp + 4]: the pc to go back to */
	stmfd sp!, {r0-r12, lr}
	ldr r0, =nspire_pagein_pc
	ldr r0, [r0]
	str r0, [sp, #60]
	mov r4, sp
	bic sp, sp, #7
	ldr r0, =nspire_pagein_address
	ldr r0, [r0]
	bl nspire_pagein
	mov sp, r4
	ldr r0, =nspire_pagein_psr
	ldr r0, [r0]
	msr cpsr_f, r0			/* the condition flags */
	ldmfd sp!, {r0-r12, lr}
	add sp, sp, #4
	ldr pc, [sp], #4

nspire_crash_trampoline:
	ldr sp, =nspire_crash_stack_top
	bl nspire_crash_report
1:	b 1b

/* unsigned long nspire_banked_get(unsigned long mode, int lr): the sp (or
lr) of the mode in the CPSR mode bits given */
	.global nspire_banked_get, nspire_banked_set, nspire_spsr
nspire_banked_get:
	mrs r3, cpsr
	and r0, r0, #0x1F
	cmp r0, #0x10			/* user mode shares system mode's registers */
	moveq r0, #0x1F
	orr r0, r0, #0xC0		/* interrupts off */
	msr cpsr_c, r0
	cmp r1, #0
	moveq r0, sp
	movne r0, lr
	msr cpsr_c, r3
	bx lr

/* void nspire_banked_set(unsigned long mode, int lr, unsigned long value) */
nspire_banked_set:
	mrs r3, cpsr
	and r0, r0, #0x1F
	cmp r0, #0x10
	moveq r0, #0x1F
	orr r0, r0, #0xC0
	msr cpsr_c, r0
	cmp r1, #0
	moveq sp, r2
	movne lr, r2
	msr cpsr_c, r3
	bx lr

/* unsigned long nspire_spsr(void): the interrupted code's CPSR */
nspire_spsr:
	mrs r0, spsr
	bx lr

	.ltorg
