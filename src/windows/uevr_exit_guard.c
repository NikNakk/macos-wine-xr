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
 * The crash itself is also prevented, wherever it happens (at quit, or while loading
 * with UEVR's Ghosting Fix): a fault inside the engine allocator's Free on a pointer it
 * does not own is skipped. Unreal's FMemory::Free loads GMalloc and calls its virtual
 * Free (slot 4); FMallocBinned::Free looks up the pointer's pool, finds none for memory
 * UEVR allocated, and reads the null pool (address 0x2). The guard checks for a near-null
 * fault in the game's code, called through "call [rax+0x20]" from a short function that
 * loads the allocator global (FMemory::Free), then unwinds
 * out of it and resumes in FMemory::Free as if the free had been done: that block leaks,
 * which is harmless, and nothing has been locked yet at that point.
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

static int readable(const void *p, SIZE_T size)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (const char *)p + size <= (const char *)mbi.BaseAddress + mbi.RegionSize;
}

/* A fault in GMalloc->Free called from FMemory::Free: resume in FMemory::Free as if the
 * free had returned. Returns 1 when the context was changed. */
static DWORD64 known_free; /* the Free already identified through FMemory::Free */

static int skip_unowned_free(CONTEXT *context, ULONG_PTR address)
{
    DWORD64 image = 0, caller_image = 0, main = (DWORD64)GetModuleHandleW(NULL);
    PRUNTIME_FUNCTION fn, caller_fn;
    CONTEXT caller = *context;
    PVOID handler_data = NULL;
    DWORD64 frame = 0, slot = 0;
    const unsigned char *call, *code;
    DWORD i;
    int loads_global = 0;
    if (address >= 0x10000 || context->Rip < main || context->Rip >= main + 0x10000000) return 0;
    fn = RtlLookupFunctionEntry(context->Rip, &image, NULL);
    if (!fn || image != main) { say("not skipped: no unwind entry in the game for %p\n", (void *)context->Rip); return 0; }
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context->Rip, fn, &caller, &handler_data, &frame, NULL);
    /* The caller made the call with "call [rax+0x20]": slot 4 of a vtable (FMalloc::Free). */
    call = (const unsigned char *)caller.Rip - 3;
    if (!readable(call, 3) || call[0] != 0xff || call[1] != 0x50 || call[2] != 0x20) {
        say("not skipped: %p returns to %p, not after call [rax+0x20]\n", (void *)context->Rip, (void *)caller.Rip);
        return 0;
    }
    caller_fn = RtlLookupFunctionEntry(caller.Rip, &caller_image, NULL);
    if (!caller_fn || caller_image != image || caller_fn->EndAddress - caller_fn->BeginAddress > 0x80) {
        say("not skipped: caller at %p is not a short function of the game\n", (void *)caller.Rip);
        return 0;
    }
    /* FMemory::Free: "mov rcx, [rip+GMalloc]" (a global holding an object) before the call. */
    code = (const unsigned char *)(image + caller_fn->BeginAddress);
    for (i = 0; i + 7 <= (DWORD)(caller.Rip - 3 - (DWORD64)code); i++) {
        const unsigned char *global;
        const DWORD64 *object, *vtable;
        if (code[i] != 0x48 || code[i + 1] != 0x8b || code[i + 2] != 0x0d) continue;
        global = code + i + 7 + *(const INT32 *)(code + i + 3);
        if (!readable(global, 8)) continue;
        object = *(const DWORD64 *const *)global;
        if (!object || !readable(object, 8)) continue;
        vtable = (const DWORD64 *)*object;
        if (!vtable || !readable(vtable + 4, 8)) continue;
        loads_global = 1;
        slot = vtable[4];
        break;
    }
    /* Also reached from Realloc(Ptr, 0), which calls Free on an inner allocator held in a
     * register and then returns null. There the function must be the Free already seen
     * through FMemory::Free, or the fault the binned pool lookup's: TableIndex (offset 2)
     * of a null FPoolInfo. */
    if (loads_global) {
        known_free = image + fn->BeginAddress;
    } else if (image + fn->BeginAddress != known_free && address != 2) {
        say("not skipped: caller at %p does not load an allocator global\n", (void *)caller.Rip);
        return 0;
    }
    /* Slot 4 is the faulting function, or a hook in front of it (UEVR hooks engine code). */
    say("skipped GMalloc->Free of a block the engine does not own (%p): fault at %p in %p, slot 4 %p, "
        "resuming at %p\n", (void *)caller.Rbx, (void *)context->Rip, (void *)(image + fn->BeginAddress),
        (void *)slot, (void *)caller.Rip);
    *context = caller;
    return 1;
}

static LONG CALLBACK guard(EXCEPTION_POINTERS *info)
{
    struct visible v = { 0 };
    const EXCEPTION_RECORD *r = info->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const ULONG_PTR address = r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0;
    if (skip_unowned_free(info->ContextRecord, address)) return EXCEPTION_CONTINUE_EXECUTION;
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
