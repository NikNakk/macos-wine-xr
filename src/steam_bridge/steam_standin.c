/* Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Stands in for the Windows Steam client while games use the Steam bridge.
 *
 * steam_api64.dll looks for a running Steam in
 * HKCU\Software\Valve\Steam\ActiveProcess (a live pid, a signed-in user and the
 * steamclient64.dll to load). This process provides those, the named objects
 * and hidden window some games look for (as Proton's steam.exe helper does),
 * and stays until `steam_standin.exe --stop`. The values it replaced are put
 * back when it stops, so a Windows Steam in the same prefix is unaffected.
 *
 *   steam_standin.exe [C:\path\to\steamclient64.dll]
 *   steam_standin.exe --stop
 *
 * MWXR_STEAM_ACTIVE_USER sets the ActiveUser value (default 1: "signed in"). */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define ACTIVE_PROCESS L"Software\\Valve\\Steam\\ActiveProcess"
#define QUIT_EVENT L"MWXR_SteamBridgeStandinQuit"

struct saved_value {
	const WCHAR *name;
	BOOL present;
	DWORD type;
	BYTE data[2048];
	DWORD size;
};

static struct saved_value saved[] = {{.name = L"pid"}, {.name = L"ActiveUser"}, {.name = L"SteamClientDll64"}};

static void save_values(HKEY key)
{
	for (size_t i = 0; i < ARRAYSIZE(saved); i++) {
		saved[i].size = sizeof(saved[i].data);
		saved[i].present =
		    !RegQueryValueExW(key, saved[i].name, NULL, &saved[i].type, saved[i].data, &saved[i].size);
	}
}

static void restore_values(void)
{
	HKEY key;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, ACTIVE_PROCESS, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL))
		return;
	for (size_t i = 0; i < ARRAYSIZE(saved); i++) {
		if (saved[i].present)
			RegSetValueExW(key, saved[i].name, 0, saved[i].type, saved[i].data, saved[i].size);
		else
			RegDeleteValueW(key, saved[i].name);
	}
	RegCloseKey(key);
}

static DWORD WINAPI steam_windows(void *arg)
{
	WNDCLASSEXW wndclass;
	MSG msg;
	(void)arg;
	ZeroMemory(&wndclass, sizeof(wndclass));
	wndclass.cbSize = sizeof(wndclass);
	wndclass.lpfnWndProc = DefWindowProcW;
	wndclass.lpszClassName = L"vguiPopupWindow";
	RegisterClassExW(&wndclass);
	CreateWindowW(wndclass.lpszClassName, L"Steam", WS_POPUP, 40, 40, 400, 300, NULL, NULL, NULL, NULL);
	while (GetMessageW(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	return 0;
}

int wmain(int argc, WCHAR **argv)
{
	const WCHAR *dll = L"C:\\windows\\system32\\steamclient64.dll";
	WCHAR user_text[32];
	DWORD pid = GetCurrentProcessId(), user = 1;
	HANDLE quit;
	HKEY key;

	if (argc > 1 && !wcscmp(argv[1], L"--stop")) {
		quit = OpenEventW(EVENT_MODIFY_STATE, FALSE, QUIT_EVENT);
		if (!quit) {
			fprintf(stderr, "steam_standin: none is running\n");
			return 1;
		}
		SetEvent(quit);
		return 0;
	}
	if (argc > 1)
		dll = argv[1];
	if (GetEnvironmentVariableW(L"MWXR_STEAM_ACTIVE_USER", user_text, ARRAYSIZE(user_text)))
		user = wcstoul(user_text, NULL, 10);

	quit = CreateEventW(NULL, TRUE, FALSE, QUIT_EVENT);
	if (!quit || GetLastError() == ERROR_ALREADY_EXISTS) {
		fprintf(stderr, "steam_standin: already running\n");
		return 1;
	}
	if (RegCreateKeyExW(HKEY_CURRENT_USER, ACTIVE_PROCESS, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL)) {
		fprintf(stderr, "steam_standin: cannot open HKCU\\%ls\n", ACTIVE_PROCESS);
		return 1;
	}
	save_values(key);
	RegSetValueExW(key, L"pid", 0, REG_DWORD, (const BYTE *)&pid, sizeof(pid));
	RegSetValueExW(key, L"ActiveUser", 0, REG_DWORD, (const BYTE *)&user, sizeof(user));
	RegSetValueExW(key, L"SteamClientDll64", 0, REG_SZ, (const BYTE *)dll, (DWORD)((wcslen(dll) + 1) * sizeof(WCHAR)));
	RegCloseKey(key);

	/* Named objects games and their launchers look for (Proton's steam.exe makes the same). */
	CreateEventW(NULL, FALSE, FALSE, L"Steam3Master_SharedMemLock");
	CreateEventW(NULL, FALSE, FALSE, L"Global\\Valve_SteamIPC_Class");
	CreateThread(NULL, 0, steam_windows, NULL, 0, NULL);

	printf("steam_standin: pid %lu, ActiveUser %lu, %ls\n", (unsigned long)pid, (unsigned long)user, dll);
	fflush(stdout);
	WaitForSingleObject(quit, INFINITE);
	restore_values();
	printf("steam_standin: stopped; previous values restored\n");
	return 0;
}
