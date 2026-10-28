// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Steam's own restart, for a client started by Steam Play.app (docs/steam-play.md, The
// Compatibility page). Built into the compat gate library.
//
// Steam restarts by ending the client with exit code 42 and leaving the new start to its
// parent: Valve's Steam.app launcher stays running for that. Steam Play.app replaces
// itself with the client instead, so that the Dock icon, and a pinned one, stay Steam
// Play.app's; nothing would start the client again. So in steam_osx this catches
// exit(42) and _exit(42), brings the game list up to date (steam_play_games.py sync, as
// Steam Play.app does) and replaces the process with steam_osx again, with the same
// arguments, as arm64. The new client keeps Steam Play.app's environment, so it loads
// this library and opens Steam Play again.
//
// exit(42) only records the restart: the start happens in an exit handler registered when
// this library loads, so it runs after Steam's own exit handlers and C++ destructors,
// which are registered later. Any other exit is left alone.

#include <crt_externs.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach/machine.h>
#include <libgen.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void mwxr_gate_log(const char *fmt, ...);

#define RESTART_EXIT_CODE 42
#define TOOL_NAME "macos-wine-xr-proton"

static int restart_requested;

static int in_client(void)
{
	char exe[PATH_MAX];
	uint32_t size = sizeof(exe);
	return _NSGetExecutablePath(exe, &size) == 0 && strcmp(basename(exe), "steam_osx") == 0;
}

// STEAM_COMPAT_TOOL_MAPPINGS from <payload>/mapped after a sync, as Steam Play.app sets it.
static void update_mappings(void)
{
	char payload[PATH_MAX], script[PATH_MAX], mapped[PATH_MAX];
	snprintf(payload, sizeof(payload), "%s/Library/Application Support/macos-wine-xr/steam-play", getenv("HOME"));
	snprintf(script, sizeof(script), "%s/steam_play_games.py", payload);
	snprintf(mapped, sizeof(mapped), "%s/mapped", payload);
	char *args[] = {"/usr/bin/python3", "-I", script, "sync", "--payload", payload, NULL};
	pid_t pid;
	int status = 0;
	if (posix_spawn(&pid, args[0], NULL, NULL, args, *_NSGetEnviron()) == 0)
		waitpid(pid, &status, 0);

	FILE *f = fopen(mapped, "r");
	if (!f)
		return;
	size_t cap = 4096, len = 0;
	char *out = calloc(cap, 1), line[256];
	while (out && fgets(line, sizeof(line), f)) {
		char *end = NULL;
		unsigned long appid = strtoul(line, &end, 10);
		if (end == line || appid == 0)
			continue;
		char entry[64];
		int n = snprintf(entry, sizeof(entry), "%s%lu 250 " TOOL_NAME, len ? "," : "", appid);
		if (len + (size_t)n + 1 > cap) {
			char *grown = realloc(out, cap *= 2);
			if (!grown)
				break;
			out = grown;
		}
		memcpy(out + len, entry, (size_t)n + 1);
		len += (size_t)n;
	}
	fclose(f);
	if (out && len)
		setenv("STEAM_COMPAT_TOOL_MAPPINGS", out, 1);
	free(out);
}

static void restart_client(void)
{
	char exe[PATH_MAX];
	uint32_t size = sizeof(exe);
	if (_NSGetExecutablePath(exe, &size) != 0)
		return;
	update_mappings();
	mwxr_gate_log("Steam restart: starting %s again", exe);
	fflush(NULL);
	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC);
	cpu_type_t arm64 = CPU_TYPE_ARM64;
	size_t set = 0;
	posix_spawnattr_setbinpref_np(&attr, 1, &arm64, &set);
	int err = posix_spawn(NULL, exe, NULL, &attr, *_NSGetArgv(), *_NSGetEnviron());
	mwxr_gate_log("Steam restart: could not start the client again (%d)", err);
}

static void after_exit_handlers(void)
{
	if (restart_requested)
		restart_client();
}

static void mwxr_exit(int status)
{
	if (status == RESTART_EXIT_CODE && in_client())
		restart_requested = 1;
	exit(status);
}

static void mwxr__exit(int status)
{
	if (status == RESTART_EXIT_CODE && in_client())
		restart_client();
	_exit(status);
}

__attribute__((used)) static const struct {
	const void *replacement, *original;
} restart_interposers[] __attribute__((section("__DATA,__interpose"))) = {
	{(const void *)mwxr_exit, (const void *)exit},
	{(const void *)mwxr__exit, (const void *)_exit},
};

__attribute__((constructor)) static void register_restart(void)
{
	if (in_client())
		atexit(after_exit_handlers);
}
