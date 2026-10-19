/* Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * void sysv_invoke(struct sysv_call *call)  (System V, x86-64)
 *
 * Calls call->fn with the general registers, vector registers and stack
 * arguments the structure holds, and stores rax and xmm0 back into it. The
 * Windows half decides which value goes where (steamclient_main.c). */

#ifdef __APPLE__
#define SYM(name) _##name
#else
#define SYM(name) name
#endif

/* struct sysv_call offsets (unixlib.h) */
#define FN      0
#define GPR     8
#define XMM     56
#define STACK   120
#define NSTACK  248
#define RET_RAX 256
#define RET_XMM 264

	.text
	.globl SYM(sysv_invoke)
	.p2align 4
SYM(sysv_invoke):
	pushq %rbp
	movq %rsp, %rbp
	pushq %rbx
	pushq %r12
	movq %rdi, %rbx

	/* Stack arguments: reserve them rounded up to 16 bytes, then copy in order. */
	movl NSTACK(%rbx), %ecx
	leaq 15(,%rcx,8), %rax
	andq $-16, %rax
	subq %rax, %rsp
	andq $-16, %rsp
	xorl %edx, %edx
1:	cmpl %ecx, %edx
	jae 2f
	movq STACK(%rbx,%rdx,8), %rax
	movq %rax, (%rsp,%rdx,8)
	incl %edx
	jmp 1b
2:
	movsd XMM+0(%rbx), %xmm0
	movsd XMM+8(%rbx), %xmm1
	movsd XMM+16(%rbx), %xmm2
	movsd XMM+24(%rbx), %xmm3
	movsd XMM+32(%rbx), %xmm4
	movsd XMM+40(%rbx), %xmm5
	movsd XMM+48(%rbx), %xmm6
	movsd XMM+56(%rbx), %xmm7
	movq GPR+8(%rbx), %rsi
	movq GPR+16(%rbx), %rdx
	movq GPR+24(%rbx), %rcx
	movq GPR+32(%rbx), %r8
	movq GPR+40(%rbx), %r9
	movq GPR+0(%rbx), %rdi
	movq FN(%rbx), %r11
	movl $8, %eax /* vector registers used, should the callee be variadic */
	callq *%r11

	movq %rax, RET_RAX(%rbx)
	movsd %xmm0, RET_XMM(%rbx)
	leaq -16(%rbp), %rsp
	popq %r12
	popq %rbx
	popq %rbp
	ret

#if defined(__linux__) && defined(__ELF__)
	.section .note.GNU-stack,"",@progbits
#endif
