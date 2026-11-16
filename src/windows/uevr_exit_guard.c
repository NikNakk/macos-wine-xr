/*
 * Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * mwxr-uevr-exit-guard.dll: loaded into a game by mwxr-uevr-inject.exe before UEVR
 * (docs/uevr.md, Quitting). Games with UEVR commonly crash on quit: during the C
 * runtime's exit, after the game's window has gone, the engine frees memory that UEVR
 * allocated outside it (The Turing Test: FMallocBinned finds no pool for the pointer
 * and reads address 0x2), and Unreal's crash handler then shows "Fatal error".
 *
 * An access violation while the process has no visible window is taken as that crash:
 * the process ends at once with exit code 0. While a window is visible (any time the
 * game is being played) nothing changes, and the guard is loaded only after the game's
 * window has appeared, so early start-up is unaffected.
 */
#include <windows.h>

static DWORD self;

static BOOL CALLBACK find_visible(HWND hwnd, LPARAM found)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == self && IsWindowVisible(hwnd)) {
        *(BOOL *)found = TRUE;
        return FALSE;
    }
    return TRUE;
}

static void say(const char *text)
{
    DWORD written;
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE) WriteFile(err, text, lstrlenA(text), &written, NULL);
}

static LONG CALLBACK guard(EXCEPTION_POINTERS *info)
{
    BOOL visible = FALSE;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    EnumWindows(find_visible, (LPARAM)&visible);
    if (visible) return EXCEPTION_CONTINUE_SEARCH;
    say("mwxr-uevr-exit-guard: access violation with no window left (UEVR's crash on quit); "
        "ending the process\n");
    TerminateProcess(GetCurrentProcess(), 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        self = GetCurrentProcessId();
        AddVectoredExceptionHandler(1, guard);
        say("mwxr-uevr-exit-guard: armed\n");
    }
    return TRUE;
}
