/* Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Runs the Steam bridge's calling-convention path natively on x86-64 Linux or
 * macOS, without Wine or Steam: a Microsoft-convention call into a proxy's
 * vtable stub (stubs.S), the frame it captures, the System V call planned from
 * a method description (bridge_core.c), and that call made (sysv_invoke.S)
 * into an ordinary function here. Every argument and result is checked. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bridge_core.h"

void sysv_invoke(struct sysv_call *call);

static int failures;
#define CHECK(cond)                                                                                                \
	do {                                                                                                           \
		if (!(cond)) {                                                                                             \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);                              \
			failures++;                                                                                            \
		}                                                                                                          \
	} while (0)

/* A stand-in for a Mac client object: the "native" methods record what they get. */
struct native_object {
	int dummy;
};
static struct native_object native;
static uint64_t seen[12];
static double seen_f[8];

static uint64_t plain(void *self, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g,
                      uint64_t h)
{
	CHECK(self == &native);
	uint64_t v[] = {a, b, c, d, e, f, g, h};
	memcpy(seen, v, sizeof(v));
	return a + b + c + d + e + f + g + h;
}

/* UpdateAvgRateStat-like: (name, float, double) and more past the registers. */
static int mixed(void *self, uint64_t a, float b, double c, uint64_t d, float e, uint64_t f, double g)
{
	CHECK(self == &native);
	seen[0] = a, seen_f[0] = b, seen_f[1] = c, seen[1] = d, seen_f[2] = e, seen[2] = f, seen_f[3] = g;
	return 7;
}

static uint64_t steam_id(void *self, uint32_t which)
{
	CHECK(self == &native);
	return 0x0110000100000000ull | which; /* a CSteamID in rax */
}

static float ping(void *self, uint64_t location)
{
	CHECK(self == &native);
	seen[0] = location;
	return 42.5f;
}

/* The proxy the test calls through: its vtable is the real stub table. */
struct proxy {
	void *const *vtbl;
};
static void *vtable[STEAM_STUB_COUNT];
static struct proxy proxy = {vtable};

static const struct steam_method *current;
static void *current_fn;

uint64_t BRIDGE_MS_ABI steam_dispatch(struct steam_frame *frame)
{
	struct sysv_call call;
	CHECK(frame->gpr[0] == (uint64_t)(uintptr_t)&proxy);
	if (!steam_plan_call(current, (uint64_t)(uintptr_t)current_fn, (uint64_t)(uintptr_t)&native, frame, &call))
		return 0xdead;
	sysv_invoke(&call);
	return steam_finish_call(current, &call, frame);
}

typedef uint64_t BRIDGE_MS_ABI (*ms_plain)(struct proxy *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                           uint64_t, uint64_t, uint64_t);
typedef int BRIDGE_MS_ABI (*ms_mixed)(struct proxy *, uint64_t, float, double, uint64_t, float, uint64_t, double);
typedef uint64_t *BRIDGE_MS_ABI (*ms_class8)(struct proxy *, uint64_t *result, uint32_t which);
typedef float BRIDGE_MS_ABI (*ms_float)(struct proxy *, uint64_t);

int main(void)
{
	for (int i = 0; i < STEAM_STUB_COUNT; i++)
		vtable[i] = (void *)(steam_stubs + i * STEAM_STUB_SIZE);

	/* A method the table does not describe: integers in, integers out. */
	current = NULL, current_fn = (void *)plain;
	uint64_t sum = ((ms_plain)vtable[5])(&proxy, 1, 2, 3, 4, 5, 6, 7, 8);
	CHECK(sum == 36);
	for (int i = 0; i < 8; i++)
		CHECK(seen[i] == (uint64_t)i + 1);

	/* Floats in xmm2 and xmm3 (positions 2, 3), in stack slots (5, 7), and ints between them. */
	static const struct steam_method mixed_method = {
		9, RET_INT, SPECIAL_NONE, 0xff, 7, {ARG_INT, ARG_F32, ARG_F64, ARG_INT, ARG_F32, ARG_INT, ARG_F64}, "mixed"};
	current = &mixed_method, current_fn = (void *)mixed;
	memset(seen, 0, sizeof(seen));
	int r = ((ms_mixed)vtable[9])(&proxy, 0x1111, 1.5f, 2.25, 0x2222, -3.75f, 0x3333, 1e300);
	CHECK(r == 7);
	CHECK(seen[0] == 0x1111 && seen[1] == 0x2222 && seen[2] == 0x3333);
	CHECK(seen_f[0] == 1.5 && seen_f[1] == 2.25 && seen_f[2] == -3.75 && seen_f[3] == 1e300);

	/* GetSteamID: Microsoft's hidden result pointer, System V's rax. */
	static const struct steam_method class8_method = {2, RET_CLASS8, SPECIAL_NONE, 0xff, 1, {ARG_INT}, "GetSteamID"};
	current = &class8_method, current_fn = (void *)steam_id;
	uint64_t id = 0;
	uint64_t *returned = ((ms_class8)vtable[2])(&proxy, &id, 0x55);
	CHECK(returned == &id);
	CHECK(id == 0x0110000100000055ull);

	/* A float result reaches the caller's xmm0. */
	static const struct steam_method float_method = {0, RET_F32, SPECIAL_NONE, 0xff, 1, {ARG_INT}, "GetLocalPingLocation"};
	current = &float_method, current_fn = (void *)ping;
	float f = ((ms_float)vtable[0])(&proxy, 0xabcdef);
	CHECK(f == 42.5f);
	CHECK(seen[0] == 0xabcdef);

	/* The last stub, and refusal of structures by value. */
	current = NULL, current_fn = (void *)plain;
	CHECK(((ms_plain)vtable[STEAM_STUB_COUNT - 1])(&proxy, 1, 1, 1, 1, 1, 1, 1, 1) == 8);
	static const struct steam_method struct_method = {1, RET_STRUCT, SPECIAL_NONE, 0xff, 0, {0}, "GetPublicIP"};
	current = &struct_method;
	CHECK(((ms_plain)vtable[1])(&proxy, 0, 0, 0, 0, 0, 0, 0, 0) == 0xdead);

	/* The generated table, against slots VeRantyx's bridge hard-codes from Alyx runs. */
	const struct steam_interface *user = steam_find_interface("SteamUser023");
	CHECK(user && steam_find_method(user, 2) && steam_find_method(user, 2)->ret == RET_CLASS8);
	const struct steam_interface *sockets = steam_find_interface("SteamNetworkingSockets012");
	CHECK(sockets && steam_find_method(sockets, 12) && steam_find_method(sockets, 12)->special == MSG_SEND);
	CHECK(sockets && steam_find_method(sockets, 14) && steam_find_method(sockets, 14)->special == MSG_RECEIVE);
	CHECK(sockets && steam_find_method(sockets, 27) && steam_find_method(sockets, 27)->special == MSG_RECEIVE);
	const struct steam_interface *utils = steam_find_interface("SteamNetworkingUtils004");
	CHECK(utils && steam_find_method(utils, 0) && steam_find_method(utils, 0)->special == MSG_ALLOCATE);
	const struct steam_interface *client = steam_find_interface("SteamClient021");
	int found_check = 0;
	for (unsigned i = 0; client && i < client->described_count; i++)
		found_check |= client->methods[i].special == FN_ALWAYS_ONE;
	CHECK(found_check);
	CHECK(!steam_find_interface("SteamNoSuchInterface001"));

	CHECK(steam_is_version_string("SteamUser023"));
	CHECK(steam_is_version_string("STEAMAPPS_INTERFACE_VERSION008"));
	CHECK(!steam_is_version_string("SteamUser"));
	CHECK(!steam_is_version_string("hello world 12"));

	if (failures)
		fprintf(stderr, "steam bridge ABI test: %d failures\n", failures);
	else
		printf("steam bridge ABI test: ok\n");
	return failures != 0;
}
