/*
 * Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * mwxr-uevr-exit-guard.dll: loaded into a game by mwxr-uevr-inject.exe before UEVR
 * (docs/uevr.md). Games with UEVR commonly crash on quit: while the game shuts down,
 * the engine frees memory that UEVR allocated outside it (The Turing Test:
 * FMallocBinned finds no pool for the pointer and reads address 0x2), and Unreal's
 * crash handler then shows "Fatal error".
 *
 * An access violation is taken as that crash, and the process ends at once with exit
 * code 0, when the game is shutting down: Windows has started detaching DLLs for the
 * process's exit, or the process has no visible window left. While the game is being
 * played neither holds, so nothing changes. The guard is loaded only after the game's
 * window has appeared, so start-up is unaffected.
 *
 * MWXR_UEVR_GUARD_LOG (a Windows path) names a file for what the guard sees.
 */
#include <windows.h>
#include <stdio.h>

static DWORD self;
static volatile LONG exiting;
static char log_path[MAX_PATH];

static void say(const char *format, ...)
{
    char line[1024];
    va_list args;
    DWORD written;
    HANDLE file;
    if (!log_path[0]) return;
    va_start(args, format);
    _vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);
    line[sizeof(line) - 1] = 0;
    file = CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, line, lstrlenA(line), &written, NULL);
    CloseHandle(file);
}

struct visible { int count; char first[256]; };

static BOOL CALLBACK find_visible(HWND hwnd, LPARAM data)
{
    struct visible *v = (struct visible *)data;
    DWORD pid = 0;
    RECT rect;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != self || !IsWindowVisible(hwnd)) return TRUE;
    /* A 1x1 or empty window is a helper, not the game's (DXMT's stand-in, say). */
    if (GetWindowRect(hwnd, &rect) && (rect.right - rect.left <= 1 || rect.bottom - rect.top <= 1)) return TRUE;
    if (!v->count++) {
        char cls[96] = "", title[96] = "";
        GetClassNameA(hwnd, cls, sizeof(cls));
        GetWindowTextA(hwnd, title, sizeof(title));
        _snprintf(v->first, sizeof(v->first) - 1, "%s \"%s\" %ldx%ld", cls, title, rect.right - rect.left,
                  rect.bottom - rect.top);
    }
    return TRUE;
}

static LONG CALLBACK guard(EXCEPTION_POINTERS *info)
{
    struct visible v = { 0 };
    const EXCEPTION_RECORD *r = info->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const ULONG_PTR address = r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0;
    EnumWindows(find_visible, (LPARAM)&v);
    /* UEVR's scans fault thousands of times on purpose (wild addresses, caught by UEVR);
     * only near-null reads, like the crash on quit, are logged while a window is up. */
    if (exiting || !v.count || address < 0x10000)
        say("access violation at %p (address %p), thread %lu, exiting=%ld, visible windows=%d%s%s\n",
            r->ExceptionAddress, (void *)address, GetCurrentThreadId(), exiting, v.count, v.count ? ", first " : "",
            v.first);
    if (!exiting && v.count) return EXCEPTION_CONTINUE_SEARCH;
    say("taking it as the crash on quit; ending the process\n");
    TerminateProcess(GetCurrentProcess(), 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        self = GetCurrentProcessId();
        if (!GetEnvironmentVariableA("MWXR_UEVR_GUARD_LOG", log_path, sizeof(log_path))) log_path[0] = 0;
        AddVectoredExceptionHandler(1, guard);
        say("armed in process %lu\n", self);
    } else if (reason == DLL_PROCESS_DETACH && reserved) {
        /* The process is exiting (not a FreeLibrary). */
        InterlockedExchange(&exiting, 1);
        say("process exit has begun\n");
    }
    return TRUE;
}
