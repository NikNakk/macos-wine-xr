/* Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * A stand-in for steamclient.dylib in native_test.c: an interface object, a
 * flat export, and a crash handler installed at load as the real client's is. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <string.h>

static void crash_handler(int sig) { (void)sig; }

__attribute__((constructor)) static void install_crash_handler(void)
{
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = crash_handler;
	sigaction(SIGSEGV, &action, NULL);
}

struct fake_iface;
struct fake_vtbl {
	uint64_t (*add)(struct fake_iface *, uint64_t, uint64_t);
	void (*warn)(struct fake_iface *, void (*hook)(int, const char *));
};
struct fake_iface {
	const struct fake_vtbl *vtbl;
};
static uint64_t add(struct fake_iface *self, uint64_t a, uint64_t b) { (void)self; return a + b; }
static void warn(struct fake_iface *self, void (*hook)(int, const char *)) { (void)self; hook(2, "warning from the client"); }
static const struct fake_vtbl vtbl = {add, warn};
static struct fake_iface iface = {&vtbl};

const char known_versions[] = "SteamFake042\0XSteamTail001";

void *CreateInterface(const char *version, int *code)
{
	if (code)
		*code = 0;
	return strcmp(version, known_versions) ? NULL : &iface;
}
