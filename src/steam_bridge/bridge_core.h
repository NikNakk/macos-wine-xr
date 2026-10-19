/* Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * The calling-convention work of the Windows half, kept free of Windows APIs
 * so tests can run it natively. */
#pragma once

#include <stdint.h>

#include "steam_methods_types.h"
#include "unixlib.h"

#if defined(__x86_64__) && !defined(_WIN32)
#define BRIDGE_MS_ABI __attribute__((ms_abi))
#else
#define BRIDGE_MS_ABI
#endif

/* What stubs.S saves of a Microsoft-convention method call. Position 0 is `this`. */
#define STEAM_FRAME_POSITIONS 12
struct steam_frame {
	uint64_t gpr[STEAM_FRAME_POSITIONS]; /* rcx, rdx, r8, r9, then the stack slots of positions 4-11 */
	uint64_t xmm[4];                     /* xmm0-xmm3: floating-point arguments at positions 0-3 */
	uint32_t index;                      /* the vtable slot called */
	uint32_t pad;
	uint64_t ret_xmm0; /* stored into xmm0 on return */
};

#define STEAM_STUB_COUNT 256
#define STEAM_STUB_SIZE 16
extern const uint8_t steam_stubs[]; /* stubs.S: STEAM_STUB_COUNT entries, STEAM_STUB_SIZE bytes apart */

/* Provided by the user of stubs.S: called with every captured call. */
uint64_t BRIDGE_MS_ABI steam_dispatch(struct steam_frame *frame);

const struct steam_interface *steam_find_interface(const char *version);
const struct steam_method *steam_find_method(const struct steam_interface *iface, unsigned index);

/* The Microsoft position of a method's parameter (0 is `this`). */
unsigned steam_param_position(const struct steam_method *method, unsigned param);

/* Fills `call` for the native object `native_this`. `method` may be NULL: integers only.
 * Returns 0 when the method cannot be bridged (a structure passed or returned by value). */
int steam_plan_call(const struct steam_method *method, uint64_t fn, uint64_t native_this,
                    const struct steam_frame *frame, struct sysv_call *call);

/* What the Microsoft caller gets back once the call is made. */
uint64_t steam_finish_call(const struct steam_method *method, const struct sysv_call *call, struct steam_frame *frame);

/* "SteamUser023", "STEAMAPPS_INTERFACE_VERSION008": an interface version string, length up to 63. */
int steam_is_version_string(const char *text);
