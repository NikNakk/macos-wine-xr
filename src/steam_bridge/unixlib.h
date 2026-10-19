/* Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * The boundary between the Windows half of the Steam bridge (steamclient64.dll)
 * and its native half (steamclient64.so), which loads the Mac Steam client's
 * steamclient.dylib in the same process. Both halves are 64-bit and share the
 * address space, so pointers cross as they are. The native half never calls
 * Windows code: anything the Mac client would call back is queued, and the
 * Windows half runs it later on a Windows thread. */
#pragma once

#include <stdint.h>

/* One System V call, as the native half makes it (sysv_invoke.S). */
struct sysv_call {
	uint64_t fn;
	uint64_t gpr[6];    /* rdi, rsi, rdx, rcx, r8, r9 */
	uint64_t xmm[8];    /* xmm0-xmm7, low 64 bits */
	uint64_t stack[16]; /* arguments past the registers, in order */
	uint32_t nstack;
	uint32_t pad;
	uint64_t ret_rax;
	uint64_t ret_xmm0;
};

enum steam_bridge_call {
	unix_init,
	unix_lookup,
	unix_invoke,
	unix_bind_hook,
	unix_next_deferred,
	unix_known_interface,
	unix_call_count,
};

struct init_params {
	char path[1024];     /* in: steamclient.dylib, or empty for the default */
	char app_id[32];     /* in: the Windows SteamAppId, which the Mac client reads natively */
	uint64_t always_one; /* out: native functions the Windows half hands to the client */
	uint64_t nothing;
	uint64_t pending;    /* out: address of the native uint32_t count of queued calls (same address space) */
	int32_t ok;
	char error[256];
};

struct lookup_params {
	const char *name; /* an export of steamclient.dylib */
	uint64_t fn;
};

/* unix_invoke takes a struct sysv_call. */

struct known_interface_params {
	const char *version;
	int32_t known;
};

enum deferred_kind { DEFERRED_INT_STRING = 1, DEFERRED_FREE_MESSAGE };

/* A Windows function the Mac client may call: it gets a native stand-in that queues the call. */
struct bind_hook_params {
	uint32_t kind;       /* DEFERRED_INT_STRING: void (*)(int, const char *);
	                      * DEFERRED_FREE_MESSAGE: void (*)(SteamNetworkingMessage_t *) */
	uint32_t pad;
	uint64_t windows_fn; /* Microsoft convention */
	uint64_t native_fn;  /* out: what the Mac client gets instead, or 0 when the slots are full */
};

#define DEFERRED_TEXT_MAX 1024
#define STEAM_MESSAGE_SIZE 216 /* sizeof(SteamNetworkingMessage_t), SDK 1.44 onwards */
#define STEAM_MESSAGE_FREE_DATA 176
#define STEAM_MESSAGE_RELEASE 184

struct deferred_call {
	uint32_t kind; /* enum deferred_kind, 0 when the queue is empty */
	int32_t value;
	uint64_t windows_fn;
	union {
		char text[DEFERRED_TEXT_MAX];
		uint8_t message[STEAM_MESSAGE_SIZE]; /* a copy: the client frees its own once the call returns */
	};
};
