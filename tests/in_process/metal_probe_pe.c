// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "wine/unixlib.h"
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void *reserved)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(module);
    return !__wine_init_unix_call();
}
__declspec(dllexport) LONG WINAPI MWXRMetalProbe(UINT operation, void *params)
{
    return WINE_UNIX_CALL(operation, params);
}
