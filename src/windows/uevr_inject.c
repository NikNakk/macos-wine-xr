/*
 * Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Injects UEVR (praydog/UEVR) into a running Unreal Engine game, as UEVR's own
 * frontend (UEVRInjector.exe, a .NET WPF app) does: the chosen VR runtime's DLL first,
 * then UEVRBackend.dll, each through LoadLibraryW on a remote thread, after writing
 * Frontend_RequestedRuntime into the game's UEVR config. Waits for the game's process
 * and its first visible window. Used by scripts/run-windows-game.zsh with MWXR_UEVR=1.
 *
 * Usage: mwxr-uevr-inject.exe --exe NAME.exe --dir UEVR_DIR [--runtime openxr|openvr]
 *        [--delay SECONDS] [--timeout SECONDS] [--nullify-plugins]
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <stdio.h>
#include <wchar.h>

static void say(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fwprintf(stderr, L"mwxr-uevr: ");
    vfwprintf(stderr, fmt, ap);
    fwprintf(stderr, L"\n");
    fflush(stderr);
    va_end(ap);
}

static DWORD find_process(const wchar_t *exe)
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = { .dwSize = sizeof(pe) };
    if (snap == INVALID_HANDLE_VALUE) return 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, exe)) { pid = pe.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

struct window_search { DWORD pid; BOOL found; };

static BOOL CALLBACK check_window(HWND hwnd, LPARAM param)
{
    struct window_search *s = (struct window_search *)param;
    DWORD pid = 0;
    RECT r;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == s->pid && IsWindowVisible(hwnd) && GetWindowRect(hwnd, &r) &&
        r.right - r.left > 64 && r.bottom - r.top > 64) {
        s->found = TRUE;
        return FALSE;
    }
    return TRUE;
}

static HMODULE remote_module(DWORD pid, const wchar_t *name)
{
    HMODULE base = NULL;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    MODULEENTRY32W me = { .dwSize = sizeof(me) };
    if (snap == INVALID_HANDLE_VALUE) return NULL;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
        if (!_wcsicmp(me.szModule, name)) { base = me.hModule; break; }
    CloseHandle(snap);
    return base;
}

/* LoadLibraryW in the target: kernel32's base there plus the function's offset here. */
static BOOL inject(HANDLE process, DWORD pid, const wchar_t *path)
{
    HMODULE local = GetModuleHandleW(L"kernel32.dll");
    HMODULE remote = remote_module(pid, L"kernel32.dll");
    FARPROC load = GetProcAddress(local, "LoadLibraryW");
    SIZE_T size = (wcslen(path) + 1) * sizeof(wchar_t);
    LPTHREAD_START_ROUTINE start;
    void *memory;
    HANDLE thread;
    DWORD code = 0;

    if (!remote) { say(L"kernel32.dll not found in process %lu", pid); return FALSE; }
    start = (LPTHREAD_START_ROUTINE)((BYTE *)remote + ((BYTE *)load - (BYTE *)local));
    memory = VirtualAllocEx(process, NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!memory || !WriteProcessMemory(process, memory, path, size, NULL)) {
        say(L"cannot write the path into process %lu (error %lu)", pid, GetLastError());
        return FALSE;
    }
    thread = CreateRemoteThread(process, NULL, 0, start, memory, 0, NULL);
    if (!thread) {
        say(L"CreateRemoteThread failed (error %lu)", GetLastError());
        return FALSE;
    }
    WaitForSingleObject(thread, 60000);
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    VirtualFreeEx(process, memory, 0, MEM_RELEASE);
    /* The exit code is the low half of the HMODULE: zero means LoadLibraryW failed. */
    if (!code && !remote_module(pid, wcsrchr(path, L'\\') + 1)) {
        say(L"LoadLibraryW(%ls) failed in the game", path);
        return FALSE;
    }
    say(L"loaded %ls", path);
    return TRUE;
}

/* %APPDATA%\UnrealVRMod\<exe name>\config.txt: set Frontend_RequestedRuntime, keep the rest. */
static void write_config(const wchar_t *exe, const wchar_t *runtime_dll)
{
    wchar_t appdata[MAX_PATH], dir[MAX_PATH], path[MAX_PATH], tmp[MAX_PATH], stem[MAX_PATH];
    char line[4096];
    FILE *in, *out;
    wchar_t *dot;

    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata))) return;
    wcsncpy(stem, exe, MAX_PATH - 1);
    stem[MAX_PATH - 1] = 0;
    if ((dot = wcsrchr(stem, L'.'))) *dot = 0;
    swprintf(dir, MAX_PATH, L"%ls\\UnrealVRMod", appdata);
    CreateDirectoryW(dir, NULL);
    swprintf(dir, MAX_PATH, L"%ls\\UnrealVRMod\\%ls", appdata, stem);
    CreateDirectoryW(dir, NULL);
    swprintf(path, MAX_PATH, L"%ls\\config.txt", dir);
    swprintf(tmp, MAX_PATH, L"%ls\\config.txt.mwxr", dir);
    if (!(out = _wfopen(tmp, L"wb"))) return;
    if ((in = _wfopen(path, L"rb"))) {
        while (fgets(line, sizeof(line), in))
            if (strncmp(line, "Frontend_RequestedRuntime=", 26)) fputs(line, out);
        fclose(in);
    }
    fprintf(out, "Frontend_RequestedRuntime=%ls\n", runtime_dll);
    fclose(out);
    MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
    say(L"config %ls: Frontend_RequestedRuntime=%ls", path, runtime_dll);
}

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *exe = NULL, *dir = NULL, *runtime = L"openxr";
    int delay = 5, timeout = 300, nullify = 0;
    const wchar_t *runtime_dll;
    wchar_t path[MAX_PATH];
    struct window_search search = { 0 };
    HANDLE process;
    DWORD pid = 0;
    int waited;

    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--exe") && i + 1 < argc) exe = argv[++i];
        else if (!wcscmp(argv[i], L"--dir") && i + 1 < argc) dir = argv[++i];
        else if (!wcscmp(argv[i], L"--runtime") && i + 1 < argc) runtime = argv[++i];
        else if (!wcscmp(argv[i], L"--delay") && i + 1 < argc) delay = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--timeout") && i + 1 < argc) timeout = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--nullify-plugins")) nullify = 1;
        else { say(L"unknown argument %ls", argv[i]); return 2; }
    }
    if (!exe || !dir) {
        say(L"usage: mwxr-uevr-inject.exe --exe NAME.exe --dir UEVR_DIR [--runtime openxr|openvr] "
            L"[--delay S] [--timeout S] [--nullify-plugins]");
        return 2;
    }
    if (!wcscmp(runtime, L"openxr")) runtime_dll = L"openxr_loader.dll";
    else if (!wcscmp(runtime, L"openvr")) runtime_dll = L"openvr_api.dll";
    else { say(L"--runtime must be openxr or openvr"); return 2; }

    for (waited = 0; waited < timeout * 4 && !(pid = find_process(exe)); waited++) Sleep(250);
    if (!pid) { say(L"%ls did not start within %d s", exe, timeout); return 1; }
    say(L"%ls is process %lu", exe, pid);
    search.pid = pid;
    for (; waited < timeout * 4; waited++) {
        EnumWindows(check_window, (LPARAM)&search);
        if (search.found || !find_process(exe)) break;
        Sleep(250);
    }
    if (!search.found) { say(L"no window from %ls; not injecting", exe); return 1; }
    say(L"window up; injecting in %d s", delay);
    Sleep(delay * 1000);

    process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                          PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
    if (!process) { say(L"OpenProcess(%lu) failed (error %lu)", pid, GetLastError()); return 1; }
    write_config(exe, runtime_dll);
    if (nullify) {
        swprintf(path, MAX_PATH, L"%ls\\UEVRPluginNullifier.dll", dir);
        inject(process, pid, path);
    }
    swprintf(path, MAX_PATH, L"%ls\\%ls", dir, runtime_dll);
    if (!inject(process, pid, path)) return 1;
    swprintf(path, MAX_PATH, L"%ls\\UEVRBackend.dll", dir);
    if (!inject(process, pid, path)) return 1;
    say(L"UEVR injected into %ls (%ls); its menu opens with Insert", exe, runtime);
    CloseHandle(process);
    return 0;
}
