/*
 * Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * mwxr-steam.exe: stands in for a Windows Steam client when games in Wine use the macOS
 * Steam client through lsteamclient (docs/lsteamclient.md).
 *
 * A game's steam_api.dll only starts when HKCU\Software\Valve\Steam\ActiveProcess names a
 * running Steam (pid) and its steamclient64.dll (SteamClientDll64). This records itself
 * as that process, starts the game, waits for it and returns its exit code. The ntdll
 * redirect (MWXR_LSTEAMCLIENT=1) then sends the game's steamclient64.dll to lsteamclient.
 *
 * Usage: mwxr-steam.exe [--steam-dir C:\path\to\Steam] -- game.exe [arguments...]
 *   --steam-dir  folder holding the Windows steamclient64.dll (default: the
 *                SteamClientDll64 already in the registry, else
 *                C:\Program Files (x86)\Steam)
 */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static const WCHAR active_key[] = L"Software\\Valve\\Steam\\ActiveProcess";

static BOOL set_dword( HKEY key, const WCHAR *name, DWORD value )
{
    return !RegSetValueExW( key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
}

static BOOL set_string( HKEY key, const WCHAR *name, const WCHAR *value )
{
    return !RegSetValueExW( key, name, 0, REG_SZ, (const BYTE *)value,
                            (DWORD)((wcslen( value ) + 1) * sizeof(WCHAR)) );
}

/* The command line after the first "--" argument, quoting kept as given. */
static WCHAR *game_command_line( WCHAR *cmdline )
{
    WCHAR *p = cmdline;
    BOOL quoted = FALSE;

    while (*p)
    {
        if (*p == '"') quoted = !quoted;
        else if (!quoted && p[0] == '-' && p[1] == '-' && (p == cmdline || p[-1] == ' ') &&
                 (p[2] == ' ' || !p[2]))
        {
            p += 2;
            while (*p == ' ') p++;
            return *p ? p : NULL;
        }
        p++;
    }
    return NULL;
}

int wmain( int argc, WCHAR **argv )
{
    WCHAR client64[MAX_PATH], client32[MAX_PATH], steam_dir[MAX_PATH] = L"";
    WCHAR *game = game_command_line( GetCommandLineW() );
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD exit_code = 1, size;
    HKEY key;
    int i;

    for (i = 1; i < argc && wcscmp( argv[i], L"--" ); i++)
        if (!wcscmp( argv[i], L"--steam-dir" ) && i + 1 < argc) lstrcpynW( steam_dir, argv[++i], MAX_PATH );

    if (!game)
    {
        fwprintf( stderr, L"usage: mwxr-steam.exe [--steam-dir DIR] -- game.exe [arguments...]\n" );
        return 2;
    }
    if (RegCreateKeyExW( HKEY_CURRENT_USER, active_key, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL ))
    {
        fwprintf( stderr, L"mwxr-steam: cannot open HKCU\\%ls\n", active_key );
        return 1;
    }

    size = sizeof(client64);
    if (steam_dir[0] || RegQueryValueExW( key, L"SteamClientDll64", NULL, NULL, (BYTE *)client64, &size ))
    {
        if (!steam_dir[0]) lstrcpynW( steam_dir, L"C:\\Program Files (x86)\\Steam", MAX_PATH );
        swprintf( client64, MAX_PATH, L"%ls\\steamclient64.dll", steam_dir );
        swprintf( client32, MAX_PATH, L"%ls\\steamclient.dll", steam_dir );
        set_string( key, L"SteamClientDll64", client64 );
        set_string( key, L"SteamClientDll", client32 );
    }
    if (GetFileAttributesW( client64 ) == INVALID_FILE_ATTRIBUTES)
        fwprintf( stderr, L"mwxr-steam: warning: %ls does not exist; steam_api cannot load it\n", client64 );
    set_dword( key, L"pid", GetCurrentProcessId() );
    RegCloseKey( key );

    if (!CreateProcessW( NULL, game, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ))
    {
        fwprintf( stderr, L"mwxr-steam: cannot start %ls (error %lu)\n", game, GetLastError() );
        return 1;
    }
    CloseHandle( pi.hThread );
    WaitForSingleObject( pi.hProcess, INFINITE );
    GetExitCodeProcess( pi.hProcess, &exit_code );
    CloseHandle( pi.hProcess );
    return (int)exit_code;
}
