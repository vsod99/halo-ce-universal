/*
START.S

The replay's start on QEMU's ARM926 board (versatilepb): a stack, main, and
QEMU's semihosting for files and the console.
*/

	.arm
	.global _start
_start:
	ldr sp, =0x07F00000
	bl main
	ldr r1, =0x20026
	mov r0, #0x18
	svc 0x123456
1:	b 1b

	/* int semihost(int operation, void *arguments) */
	.global semihost
semihost:
	svc 0x123456
	bx lr
