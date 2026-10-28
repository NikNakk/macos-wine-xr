/* Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Read-only integration probe; build against the pinned, externally fetched
 * Steamworks headers used by lsteamclient. No SDK files are redistributed. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "steam_api.h"

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: steam_bridge_probe.exe steamclient-DLL\n"); return 2; }
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) { fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError()); return 1; }
    auto factory = reinterpret_cast<void *(__cdecl *)(const char *, int *)>(GetProcAddress(dll, "CreateInterface"));
    auto next = reinterpret_cast<bool (__cdecl *)(HSteamPipe, CallbackMsg_t *, int *)>(GetProcAddress(dll, "Steam_BGetCallback"));
    auto release = reinterpret_cast<bool (__cdecl *)(HSteamPipe)>(GetProcAddress(dll, "Steam_FreeLastCallback"));
    if (!factory || !next || !release || !GetModuleHandleA("lsteamclient.dll")) {
        fprintf(stderr, "Steam DLL was not redirected to lsteamclient\n"); return 1;
    }
    auto client = static_cast<ISteamClient *>(factory(STEAMCLIENT_INTERFACE_VERSION, nullptr));
    if (!client) { fprintf(stderr, "CreateInterface failed\n"); return 1; }
    HSteamPipe pipe = client->CreateSteamPipe();
    HSteamUser user = pipe ? client->ConnectToGlobalUser(pipe) : 0;
    if (!pipe || !user) { fprintf(stderr, "No native Steam connection\n"); return 1; }
    auto utils = client->GetISteamUtils(pipe, STEAMUTILS_INTERFACE_VERSION);
    auto stats = client->GetISteamUserStats(user, pipe, STEAMUSERSTATS_INTERFACE_VERSION);
    auto steam_user = client->GetISteamUser(user, pipe, STEAMUSER_INTERFACE_VERSION);
    auto friends = client->GetISteamFriends(user, pipe, STEAMFRIENDS_INTERFACE_VERSION);
    if (!utils || !stats || !steam_user || !friends) { fprintf(stderr, "Missing interfaces\n"); return 1; }
    // Exercise scalar, 64-bit structure return, and native string-to-Windows pointer conversion.
    const char *persona = friends->GetPersonaName();
    // MinGW's C++ small-class return convention differs from the MSVC ABI used
    // by Steamworks games. Call the explicit this/return-buffer ABI used by the
    // generated bridge thunk, rather than testing MinGW's class-return ABI.
    CSteamID id;
#ifdef __i386__
    using GetID = CSteamID *(__attribute__((thiscall)) *)(void *, CSteamID *);
#else
    using GetID = CSteamID *(*)(void *, CSteamID *);
#endif
    auto get_id = reinterpret_cast<GetID>((*reinterpret_cast<void ***>(steam_user))[2]);
    get_id(steam_user, &id);
    printf("identity checks: persona=%d, valid_id=%d, logged_on=%d, app=%u\n",
           persona && *persona, id.IsValid(), steam_user->BLoggedOn(), utils->GetAppID());
    if (!persona || !*persona || !id.IsValid() || !utils->GetAppID()) {
        fprintf(stderr, "Invalid identity/app/string results\n"); return 1;
    }
    printf("pointer_bits=%u: redirected, connected, identity/string/app queries PASS (app=%u)\n",
           unsigned(sizeof(void *) * 8), utils->GetAppID());
    stats->RequestCurrentStats();
    SteamAPICall_t call = stats->GetNumberOfCurrentPlayers();
    unsigned callbacks = 0, stats_callbacks = 0;
    bool async_done = false;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < 10000) {
        CallbackMsg_t msg = {};
        int ignored = 0;
        while (next(pipe, &msg, &ignored)) {
            ++callbacks;
            if (msg.m_iCallback == UserStatsReceived_t::k_iCallback) {
                // macOS Steam can omit the Windows structure's trailing padding.
                // Require every field, while accepting either padding convention.
                const size_t fields_size = offsetof(UserStatsReceived_t, m_steamIDUser) + sizeof(CSteamID);
                if (!msg.m_pubParam || msg.m_cubParam < int(fields_size) || msg.m_cubParam > int(sizeof(UserStatsReceived_t))) {
                    fprintf(stderr, "Bad converted stats callback size: %d\n", msg.m_cubParam); return 1;
                }
                UserStatsReceived_t result = {};
                memcpy(&result, msg.m_pubParam, msg.m_cubParam);
                if (result.m_nGameID != utils->GetAppID() || result.m_steamIDUser != id) { fprintf(stderr, "Bad callback game ID\n"); return 1; }
                ++stats_callbacks;
                printf("stats callback: converted payload PASS (result=%d)\n", int(result.m_eResult));
            }
            release(pipe);
        }
        bool failed = false;
        if (!async_done && call && utils->IsAPICallCompleted(call, &failed)) {
            NumberOfCurrentPlayers_t result = {};
            bool got = utils->GetAPICallResult(call, &result, sizeof(result), result.k_iCallback, &failed);
            printf("async result: completed, retrieved=%d, failed=%d\n", got, failed);
            async_done = got && !failed;
        }
        if (stats_callbacks && async_done) break;
        Sleep(20);
    }
    client->ReleaseUser(pipe, user);
    bool released = client->BReleaseSteamPipe(pipe);
    printf("callbacks=%u, stats_callbacks=%u, async_success=%d, pipe_released=%d\n",
           callbacks, stats_callbacks, async_done, released);
    // A disconnected/offline account is reported as incomplete, not a passing callback test.
    return stats_callbacks && async_done && released ? 0 : 3;
}
