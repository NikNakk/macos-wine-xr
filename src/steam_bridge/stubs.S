/* Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Vtable entries of the Windows half's interface proxies (Microsoft x64
 * convention). Entry n is STEAM_STUB_SIZE bytes after steam_stubs: it puts n
 * in eax and joins steam_stub_common, which saves every register and stack
 * slot an argument may be in (struct steam_frame, bridge_core.h) and calls
 * steam_dispatch(frame). The float register is reloaded on return for methods
 * that return a float. */

#if defined(__APPLE__)
#define SYM(name) _##name
#else
#define SYM(name) name
#endif

#ifdef _WIN32
#define SEH(...) __VA_ARGS__
#else
#define SEH(...)
#endif

#define SHADOW 32
#define FRAME 144 /* sizeof(struct steam_frame) */
#define F(off) (SHADOW + (off))(%rsp)

	.text
	.p2align 4
SEH(.seh_proc steam_stub_common)
steam_stub_common:
	pushq %rbp
	SEH(.seh_pushreg %rbp)
	movq %rsp, %rbp
	SEH(.seh_setframe %rbp, 0)
	subq $(SHADOW + FRAME), %rsp
	SEH(.seh_stackalloc SHADOW + FRAME)
	SEH(.seh_endprologue)
	movq %rcx, F(0)
	movq %rdx, F(8)
	movq %r8, F(16)
	movq %r9, F(24)
	/* Positions 4-11: the caller's stack slots, 40 bytes above the return address. */
	.irp n, 0, 1, 2, 3, 4, 5, 6, 7
	movq (48 + 8 * \n)(%rbp), %r10
	movq %r10, F(32 + 8 * \n)
	.endr
	movsd %xmm0, F(96)
	movsd %xmm1, F(104)
	movsd %xmm2, F(112)
	movsd %xmm3, F(120)
	movl %eax, F(128)
	movq $0, F(136)
	leaq F(0), %rcx
	callq SYM(steam_dispatch)
	movsd F(136), %xmm0
	movq %rbp, %rsp
	popq %rbp
	ret
SEH(.seh_endproc)

	.globl SYM(steam_stubs)
	.p2align 4
SYM(steam_stubs):
	.set stub_index, 0
	.rept 256 /* STEAM_STUB_COUNT */
	.p2align 4 /* STEAM_STUB_SIZE */
	movl $stub_index, %eax
	jmp steam_stub_common
	.set stub_index, stub_index + 1
	.endr

#if defined(__linux__) && defined(__ELF__)
	.section .note.GNU-stack,"",@progbits
#endif
