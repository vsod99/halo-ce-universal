/*
NSPIRE_SWITCH.S

The context switch of the Nspire port's cooperative threads
(nspire_threads.c): saves the callee-saved registers and the return address
on the current stack, stores the stack pointer, loads the other thread's and
returns into it. The soft-float ABI keeps no floating-point registers.

void nspire_thread_switch(unsigned long **save_stack, unsigned long *load_stack);
*/

	.syntax unified
	.arm
	.text
	.global nspire_thread_switch
	.type nspire_thread_switch, %function
nspire_thread_switch:
	push {r4-r11, lr}
	str sp, [r0]
	mov sp, r1
	pop {r4-r11, pc}
	.size nspire_thread_switch, . - nspire_thread_switch

/*
The game's own stack (nspire_main.c): Halo's AI keeps 80-170 KB arrays on
the stack, more than the OS gives a program, so the game runs on one from
the heap. exit() (wrapped) goes back to the OS's stack first, so that the
handlers it runs can free the game's.

int nspire_call_on_stack(int (*function)(void), void *stack_top);
*/

	.global nspire_call_on_stack
	.type nspire_call_on_stack, %function
nspire_call_on_stack:
	push {r4, lr}
	ldr r2, =nspire_os_stack_pointer
	str sp, [r2]
	mov sp, r1
	blx r0
	ldr r2, =nspire_os_stack_pointer
	ldr sp, [r2]
	mov r1, #0
	str r1, [r2]
	pop {r4, pc}
	.size nspire_call_on_stack, . - nspire_call_on_stack

	.global __wrap_exit
	.type __wrap_exit, %function
__wrap_exit:
	ldr r2, =nspire_os_stack_pointer
	ldr r1, [r2]
	cmp r1, #0
	movne sp, r1
	movne r1, #0
	strne r1, [r2]
	b __real_exit
	.size __wrap_exit, . - __wrap_exit

	.bss
	.align 2
	.global nspire_os_stack_pointer
nspire_os_stack_pointer:
	.space 4
