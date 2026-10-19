/* Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * The native half of the Steam bridge (steamclient_unix.c), called as the
 * Windows half calls it, against fake_client.c instead of steamclient.dylib.
 * Usage: native_test /path/to/fake_client.so */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "unixlib.h"

typedef long NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)(void *args);
extern const unixlib_entry_t __wine_unix_call_funcs[];

static int failures;
#define CHECK(cond)                                                                                                \
	do {                                                                                                           \
		if (!(cond)) {                                                                                             \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);                              \
			failures++;                                                                                            \
		}                                                                                                          \
	} while (0)

static void wine_handler(int sig) { (void)sig; }

int main(int argc, char **argv)
{
	struct sigaction action = {0}, now;
	struct init_params init = {0};
	struct lookup_params lookup = {.name = "CreateInterface"};
	struct sysv_call call = {0};
	struct bind_hook_params bind = {.kind = DEFERRED_INT_STRING, .windows_fn = 0x1234};
	struct bind_hook_params bind_free = {.kind = DEFERRED_FREE_MESSAGE, .windows_fn = 0x5678};
	struct known_interface_params known;
	static struct deferred_call deferred;
	uint8_t message[STEAM_MESSAGE_SIZE];

	if (argc < 2)
		return 2;
	action.sa_handler = wine_handler;
	sigaction(SIGSEGV, &action, NULL);

	snprintf(init.path, sizeof(init.path), "%s", argv[1]);
	CHECK(__wine_unix_call_funcs[unix_init](&init) == 0 && init.ok);
	sigaction(SIGSEGV, NULL, &now);
	CHECK(now.sa_handler == wine_handler); /* the client's handler was taken back out */
	CHECK(init.always_one && ((uint32_t(*)(int32_t))(uintptr_t)init.always_one)(5) == 1);

	CHECK(__wine_unix_call_funcs[unix_lookup](&lookup) == 0 && lookup.fn);
	call.fn = lookup.fn;
	call.gpr[0] = (uint64_t)(uintptr_t)"SteamFake042";
	__wine_unix_call_funcs[unix_invoke](&call);
	uint64_t object = call.ret_rax;
	CHECK(object);

	/* A method through the object's vtable, as steam_dispatch makes it. */
	uint64_t *vtbl = *(uint64_t **)(uintptr_t)object;
	memset(&call, 0, sizeof(call));
	call.fn = vtbl[0];
	call.gpr[0] = object, call.gpr[1] = 40, call.gpr[2] = 2;
	__wine_unix_call_funcs[unix_invoke](&call);
	CHECK(call.ret_rax == 42);

	/* A Windows hook the client calls: queued, with its text copied. */
	const volatile uint32_t *pending = (const volatile uint32_t *)(uintptr_t)init.pending;
	CHECK(pending && *pending == 0);
	CHECK(__wine_unix_call_funcs[unix_bind_hook](&bind) == 0 && bind.native_fn);
	memset(&call, 0, sizeof(call));
	call.fn = vtbl[1];
	call.gpr[0] = object, call.gpr[1] = bind.native_fn;
	__wine_unix_call_funcs[unix_invoke](&call);
	CHECK(*pending == 1);
	CHECK(__wine_unix_call_funcs[unix_next_deferred](&deferred) == 0);
	CHECK(*pending == 0);
	CHECK(deferred.kind == DEFERRED_INT_STRING && deferred.value == 2 && deferred.windows_fn == 0x1234);
	CHECK(!strcmp(deferred.text, "warning from the client"));
	CHECK(__wine_unix_call_funcs[unix_next_deferred](&deferred) == 0 && deferred.kind == 0);

	/* The same Windows function twice gets the same slot. */
	struct bind_hook_params again = bind;
	CHECK(__wine_unix_call_funcs[unix_bind_hook](&again) == 0 && again.native_fn == bind.native_fn);

	/* A message's free function: called with a copy of the message. */
	CHECK(__wine_unix_call_funcs[unix_bind_hook](&bind_free) == 0 && bind_free.native_fn);
	for (unsigned i = 0; i < sizeof(message); i++)
		message[i] = (uint8_t)i;
	((void (*)(void *))(uintptr_t)bind_free.native_fn)(message);
	memset(message, 0, sizeof(message)); /* the client frees its own */
	CHECK(__wine_unix_call_funcs[unix_next_deferred](&deferred) == 0);
	CHECK(deferred.kind == DEFERRED_FREE_MESSAGE && deferred.windows_fn == 0x5678);
	CHECK(deferred.message[0] == 0 && deferred.message[100] == 100 && deferred.message[215] == 215);

	/* Interface versions the client's binary carries, whole strings only. */
	known.version = "SteamFake042";
	CHECK(__wine_unix_call_funcs[unix_known_interface](&known) == 0 && known.known == 1);
	known.version = "SteamFake04";
	CHECK(__wine_unix_call_funcs[unix_known_interface](&known) == 0 && known.known == 0);
	known.version = "SteamTail001"; /* only inside "XSteamTail001" */
	CHECK(__wine_unix_call_funcs[unix_known_interface](&known) == 0 && known.known == 0);

	if (failures)
		fprintf(stderr, "steam bridge native test: %d failures\n", failures);
	else
		printf("steam bridge native test: ok\n");
	return failures != 0;
}
