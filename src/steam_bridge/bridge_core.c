/* Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "bridge_core.h"
#include "steam_methods.h"

_Static_assert(offsetof(struct steam_frame, xmm) == 96, "stubs.S offsets");
_Static_assert(offsetof(struct steam_frame, index) == 128, "stubs.S offsets");
_Static_assert(offsetof(struct steam_frame, ret_xmm0) == 136, "stubs.S offsets");
_Static_assert(sizeof(struct steam_frame) == 144, "stubs.S frame size");

static int compare_version(const void *key, const void *entry)
{
	return strcmp((const char *)key, ((const struct steam_interface *)entry)->version);
}

const struct steam_interface *steam_find_interface(const char *version)
{
	return bsearch(version, steam_interfaces, sizeof(steam_interfaces) / sizeof(steam_interfaces[0]),
	               sizeof(steam_interfaces[0]), compare_version);
}

const struct steam_method *steam_find_method(const struct steam_interface *iface, unsigned index)
{
	if (!iface)
		return NULL;
	for (unsigned i = 0; i < iface->described_count; i++)
		if (iface->methods[i].index == index)
			return &iface->methods[i];
	return NULL;
}

unsigned steam_param_position(const struct steam_method *method, unsigned param)
{
	/* Microsoft passes a by-value class result's address right after `this`. */
	return param + 1 + (method && method->ret == RET_CLASS8);
}

static void push_int(struct sysv_call *call, unsigned *ngpr, uint64_t value)
{
	if (*ngpr < 6)
		call->gpr[(*ngpr)++] = value;
	else if (call->nstack < 16)
		call->stack[call->nstack++] = value;
}

static void push_float(struct sysv_call *call, unsigned *nxmm, uint64_t bits)
{
	if (*nxmm < 8)
		call->xmm[(*nxmm)++] = bits;
	else if (call->nstack < 16)
		call->stack[call->nstack++] = bits;
}

int steam_plan_call(const struct steam_method *method, uint64_t fn, uint64_t native_this,
                    const struct steam_frame *frame, struct sysv_call *call)
{
	unsigned ngpr = 0, nxmm = 0;
	unsigned nparams = method ? method->nargs : STEAM_FRAME_POSITIONS - 1;

	memset(call, 0, sizeof(*call));
	call->fn = fn;
	if (method && method->ret == RET_STRUCT)
		return 0;
	push_int(call, &ngpr, native_this);
	for (unsigned param = 0; param < nparams; param++) {
		unsigned position = steam_param_position(method, param);
		uint8_t kind = method ? method->args[param] : ARG_INT;
		if (position >= STEAM_FRAME_POSITIONS || kind == ARG_STRUCT)
			return 0;
		if (kind == ARG_INT)
			push_int(call, &ngpr, frame->gpr[position]);
		else /* ARG_F32 and ARG_F64: the low bits of the register or slot, as in System V */
			push_float(call, &nxmm, position < 4 ? frame->xmm[position] : frame->gpr[position]);
	}
	return 1;
}

uint64_t steam_finish_call(const struct steam_method *method, const struct sysv_call *call, struct steam_frame *frame)
{
	frame->ret_xmm0 = call->ret_xmm0;
	if (method && method->ret == RET_CLASS8) {
		uint64_t *result = (uint64_t *)(uintptr_t)frame->gpr[1];
		*result = call->ret_rax;
		return (uint64_t)(uintptr_t)result;
	}
	return call->ret_rax;
}

int steam_is_version_string(const char *text)
{
	size_t len = 0;
	while (len < 64 && text[len])
		len++;
	if (len < 8 || len > 63)
		return 0;
	if (strncmp(text, "Steam", 5) && strncmp(text, "STEAM", 5))
		return 0;
	if (text[len - 1] < '0' || text[len - 1] > '9' || text[len - 2] < '0' || text[len - 2] > '9')
		return 0;
	for (size_t i = 0; i < len; i++) {
		char c = text[i];
		if (!(c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
			return 0;
	}
	return 1;
}
