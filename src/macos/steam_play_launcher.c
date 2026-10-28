// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Starts macOS Steam with Steam Play on (docs/steam-play.md). This is the executable of
// "Steam Play.app", installed by scripts/install-steam-play.zsh. It replaces itself with
// Valve's own client, unmodified, adding three variables:
//
//   DYLD_INSERT_LIBRARIES           the compat gate library (steam_compat_gate.c)
//   STEAM_EXTRA_COMPAT_TOOLS_PATHS  the folder holding the macos-wine-xr Steam Play tool
//   STEAM_COMPAT_TOOL_MAPPINGS      "APPID 250 macos-wine-xr,..." for each app ID listed in
//                                   the payload's "games" file (250 is the lowest priority
//                                   the client applies). Not the "0" catch-all: that also
//                                   moves games with a macOS build to their Windows depots.
//
// Valve's outer Steam.app has the hardened runtime, which drops DYLD_* variables; the
// client it starts, steam_osx in ~/Library/Application Support/Steam, does not. So this
// starts steam_osx directly, and as arm64: the gate is only found in the arm64 client.
// If Steam is already running, the new client hands over to it and exits, as a second
// click on Steam.app does. If the client has never been unpacked, it opens Steam.app.

#include <errno.h>
#include <limits.h>
#include <mach/machine.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

#define TOOL_NAME "macos-wine-xr"

static int exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

// Prepends value to a colon-separated variable unless it is already listed.
static void prepend(const char *name, const char *value)
{
	const char *old = getenv(name);
	if (!old || !*old) {
		setenv(name, value, 1);
		return;
	}
	size_t n = strlen(value);
	for (const char *p = old; p; p = strchr(p, ':')) {
		if (*p == ':')
			p++;
		if (strncmp(p, value, n) == 0 && (p[n] == ':' || p[n] == '\0'))
			return;
	}
	char *merged = NULL;
	if (asprintf(&merged, "%s:%s", value, old) < 0)
		return;
	setenv(name, merged, 1);
	free(merged);
}

// "APPID 250 macos-wine-xr" for each app ID in <payload>/games (one per line, # comments),
// joined with commas as the client splits them. NULL if there is no list.
static char *read_mappings(const char *payload)
{
	char path[PATH_MAX], line[256];
	snprintf(path, sizeof(path), "%s/games", payload);
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	size_t cap = 4096, len = 0;
	char *out = calloc(cap, 1);
	while (out && fgets(line, sizeof(line), f)) {
		char *end = NULL;
		unsigned long appid = strtoul(line, &end, 10);
		if (end == line || appid == 0)
			continue;
		char entry[64];
		int n = snprintf(entry, sizeof(entry), "%s%lu 250 " TOOL_NAME, len ? "," : "", appid);
		if (len + (size_t)n + 1 > cap) {
			char *grown = realloc(out, cap *= 2);
			if (!grown) {
				free(out);
				out = NULL;
				break;
			}
			out = grown;
		}
		memcpy(out + len, entry, (size_t)n + 1);
		len += (size_t)n;
	}
	fclose(f);
	return out;
}

int main(int argc, char **argv)
{
	const char *home = getenv("HOME");
	if (!home || !*home) {
		fprintf(stderr, "Steam Play: HOME is not set\n");
		return 1;
	}
	char payload[PATH_MAX], gate[PATH_MAX], tools[PATH_MAX], client[PATH_MAX];
	snprintf(payload, sizeof(payload), "%s/Library/Application Support/macos-wine-xr/steam-play", home);
	snprintf(gate, sizeof(gate), "%s/libmwxr_steam_compat_gate.dylib", payload);
	snprintf(tools, sizeof(tools), "%s/compatibilitytools.d", payload);
	snprintf(client, sizeof(client),
	         "%s/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steam_osx", home);

	if (!exists(client)) {
		// Steam.app unpacks the client on its first run; Steam Play can start after that.
		fprintf(stderr, "Steam Play: %s is missing; opening Steam.app\n", client);
		execl("/usr/bin/open", "open", "-b", "com.valvesoftware.steam", (char *)NULL);
		return 1;
	}
	if (!exists(gate) || !exists(tools)) {
		fprintf(stderr, "Steam Play: %s is incomplete; run scripts/install-steam-play.zsh\n", payload);
		return 1;
	}

	prepend("DYLD_INSERT_LIBRARIES", gate);
	prepend("STEAM_EXTRA_COMPAT_TOOLS_PATHS", tools);
	char *mappings = read_mappings(payload);
	if (mappings && *mappings)
		setenv("STEAM_COMPAT_TOOL_MAPPINGS", mappings, 1);
	else
		unsetenv("STEAM_COMPAT_TOOL_MAPPINGS");
	free(mappings);

	if (argc > 1 && strcmp(argv[1], "--print-env") == 0) {
		printf("DYLD_INSERT_LIBRARIES=%s\n", getenv("DYLD_INSERT_LIBRARIES"));
		printf("STEAM_EXTRA_COMPAT_TOOLS_PATHS=%s\n", getenv("STEAM_EXTRA_COMPAT_TOOLS_PATHS"));
		const char *mapped = getenv("STEAM_COMPAT_TOOL_MAPPINGS");
		printf("STEAM_COMPAT_TOOL_MAPPINGS=%s\n", mapped ? mapped : "(none)");
		printf("exec %s\n", client);
		return 0;
	}

	// Pass on any arguments, but not the -psn_ one older LaunchServices adds.
	char **args = calloc((size_t)argc + 1, sizeof(char *));
	if (!args)
		return 1;
	int n = 0;
	args[n++] = client;
	for (int i = 1; i < argc; i++)
		if (strncmp(argv[i], "-psn_", 5) != 0)
			args[n++] = argv[i];
	args[n] = NULL;

	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC);
	cpu_type_t arm64 = CPU_TYPE_ARM64;
	size_t set = 0;
	posix_spawnattr_setbinpref_np(&attr, 1, &arm64, &set);
	int err = posix_spawn(NULL, client, NULL, &attr, args, environ);
	fprintf(stderr, "Steam Play: could not start %s: %s\n", client, strerror(err));
	return 1;
}
