/* Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Runs under Wine against the bridge and the Mac Steam client (signed in):
 * the calls steam_api64.dll makes first, then one of each kind of call the
 * bridge converts. Not part of ctest: needs Wine, Steam and a sign-in.
 *
 *   SteamAppId=546560 wine steam_bridge_smoke.exe */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

typedef void *(__cdecl *create_interface_fn)(const char *, int *);
typedef BOOL(__cdecl *is_known_fn)(const char *);

/* Slots from the Steamworks headers (SteamClient023, SteamUtils010, SteamUser023, SteamNetworkingUtils004). */
#define CLIENT_CREATE_STEAM_PIPE 0
#define CLIENT_CONNECT_TO_GLOBAL_USER 2
#define CLIENT_GET_GENERIC_INTERFACE 12
#define UTILS_GET_APP_ID 9
#define USER_GET_STEAM_ID 2
#define NETUTILS_GET_LOCAL_PING_LOCATION 2

static void *method(void *object, int index)
{
	return (*(void ***)object)[index];
}

int main(void)
{
	HMODULE module = LoadLibraryA("steamclient64.dll");
	create_interface_fn create = module ? (create_interface_fn)(void *)GetProcAddress(module, "CreateInterface") : NULL;
	is_known_fn is_known = module ? (is_known_fn)(void *)GetProcAddress(module, "Steam_IsKnownInterface") : NULL;
	const char *client_version = "SteamClient023";
	char expected_text[32];
	int failed = 0;

	if (!create || !is_known) {
		printf("steamclient64.dll: not loaded or missing exports -> FAILED\n");
		return 1;
	}
	void *client = create(client_version, NULL);
	if (!client)
		client = create(client_version = "SteamClient021", NULL);
	if (!client) {
		printf("CreateInterface(SteamClient023/021): NULL -> FAILED (is Mac Steam running and signed in?)\n");
		return 1;
	}
	printf("%s: %p, Steam_IsKnownInterface(SteamUser023) = %d\n", client_version, client, is_known("SteamUser023"));

	int32_t pipe = ((int32_t(*)(void *))method(client, CLIENT_CREATE_STEAM_PIPE))(client);
	int32_t user = ((int32_t(*)(void *, int32_t))method(client, CLIENT_CONNECT_TO_GLOBAL_USER))(client, pipe);
	printf("pipe %d, user %d\n", pipe, user);
	failed |= !pipe || !user;

	void *(*generic)(void *, int32_t, int32_t, const char *) = method(client, CLIENT_GET_GENERIC_INTERFACE);
	void *utils = generic(client, user, pipe, "SteamUtils010");
	void *steam_user = generic(client, user, pipe, "SteamUser023");
	void *net_utils = generic(client, user, pipe, "SteamNetworkingUtils004");

	/* Integers only. */
	uint32_t app = utils ? ((uint32_t(*)(void *))method(utils, UTILS_GET_APP_ID))(utils) : 0;
	DWORD n = GetEnvironmentVariableA("SteamAppId", expected_text, sizeof(expected_text));
	printf("SteamUtils010 %p: GetAppID = %u (SteamAppId %s)\n", utils, app, n ? expected_text : "unset");
	failed |= !utils || (n && app != strtoul(expected_text, NULL, 10));

	/* A class returned by value: CSteamID through Microsoft's hidden pointer. */
	uint64_t id = 0;
	if (steam_user)
		((uint64_t * (*)(void *, uint64_t *)) method(steam_user, USER_GET_STEAM_ID))(steam_user, &id);
	printf("SteamUser023 %p: GetSteamID = %llu\n", steam_user, (unsigned long long)id);
	failed |= !steam_user || !id;

	/* A float result (the location data's age in seconds, or -1 before the relay network is up). */
	static uint8_t location[512];
	float age = net_utils ? ((float (*)(void *, void *))method(net_utils, NETUTILS_GET_LOCAL_PING_LOCATION))(net_utils, location)
	                      : -2.0f;
	printf("SteamNetworkingUtils004 %p: GetLocalPingLocation = %.2f\n", net_utils, age);
	failed |= !net_utils || age < -1.5f;

	printf("steam bridge smoke: %s\n", failed ? "FAILED" : "ok");
	return failed;
}
