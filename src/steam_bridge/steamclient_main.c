/* Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * steamclient64.dll for Wine on macOS, forwarding to the Mac Steam client.
 *
 * A Windows game's steam_api64.dll loads steamclient64.dll from the running
 * Steam (the path in HKCU\Software\Valve\Steam\ActiveProcess) and asks it for
 * interfaces and callbacks. This builtin hands it proxies of the Mac client's
 * own objects, loaded by the native half (steamclient_unix.c), so the game
 * uses the Steam session signed in on the Mac and no Windows Steam client runs
 * under Wine. The approach follows VeRantyx's VXRuntime bridge (MIT); the
 * per-method calling-convention table and the queued callbacks follow Proton's
 * lsteamclient. See docs/steam-bridge.md. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <windef.h>
#include <winbase.h>
#include <winternl.h>

#include "wine/debug.h"
#include "wine/unixlib.h"

#include "bridge_core.h"
#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(steamclient);

#define STEAM_CALL(code, params) WINE_UNIX_CALL(unix_##code, params)

static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
static BOOL loaded;
static uint64_t native_always_one, native_nothing;
static const volatile uint32_t *native_pending; /* calls the native half has queued */

static BOOL WINAPI load_client(INIT_ONCE *once, void *param, void **context)
{
	struct init_params params = {0};
	(void)once, (void)param, (void)context;
	if (!GetEnvironmentVariableA("MWXR_STEAM_CLIENT", params.path, sizeof(params.path)))
		params.path[0] = 0;
	if (!GetEnvironmentVariableA("SteamAppId", params.app_id, sizeof(params.app_id)) &&
	    !GetEnvironmentVariableA("SteamGameId", params.app_id, sizeof(params.app_id)))
		params.app_id[0] = 0;
	if (STEAM_CALL(init, &params) || !params.ok) {
		ERR("cannot load the Mac Steam client: %s\n", params.error);
		return TRUE;
	}
	native_always_one = params.always_one;
	native_nothing = params.nothing;
	native_pending = (const volatile uint32_t *)(uintptr_t)params.pending;
	loaded = TRUE;
	TRACE("Mac Steam client loaded\n");
	return TRUE;
}

static BOOL ensure_loaded(void)
{
	InitOnceExecuteOnce(&init_once, load_client, NULL, NULL);
	return loaded;
}

static uint64_t native_symbol(const char *name)
{
	struct lookup_params params = {.name = name};
	if (!ensure_loaded() || STEAM_CALL(lookup, &params))
		return 0;
	return params.fn;
}

static uint64_t call_native(uint64_t fn, const uint64_t *args, unsigned count)
{
	struct sysv_call call = {.fn = fn};
	for (unsigned i = 0; i < count; i++) {
		if (i < 6)
			call.gpr[i] = args[i];
		else
			call.stack[call.nstack++] = args[i];
	}
	STEAM_CALL(invoke, &call);
	return call.ret_rax;
}

/* ---- calls the Mac client queued for Windows code ------------------------------------------ */

static DWORD running_deferred = TLS_OUT_OF_INDEXES;

static void run_deferred(void)
{
	struct deferred_call *call;
	if (!native_pending || !__atomic_load_n(native_pending, __ATOMIC_ACQUIRE))
		return;
	if (running_deferred == TLS_OUT_OF_INDEXES || TlsGetValue(running_deferred))
		return; /* a queued call that calls Steam again does not start another round */
	TlsSetValue(running_deferred, (void *)1);
	call = HeapAlloc(GetProcessHeap(), 0, sizeof(*call));
	while (call && !STEAM_CALL(next_deferred, call) && call->kind) {
		if (call->kind == DEFERRED_INT_STRING) {
			void(__cdecl * fn)(int, const char *) = (void *)(uintptr_t)call->windows_fn;
			fn(call->value, call->text);
		} else if (call->kind == DEFERRED_FREE_MESSAGE) {
			void(__cdecl * fn)(void *) = (void *)(uintptr_t)call->windows_fn;
			fn(call->message);
		}
	}
	HeapFree(GetProcessHeap(), 0, call);
	TlsSetValue(running_deferred, NULL);
}

static uint64_t bind_hook(uint32_t kind, uint64_t windows_fn)
{
	struct bind_hook_params params = {.kind = kind, .windows_fn = windows_fn};
	if (!windows_fn)
		return 0;
	if (STEAM_CALL(bind_hook, &params) || !params.native_fn)
		WARN("no free hook slot for %p\n", (void *)(uintptr_t)windows_fn);
	return params.native_fn;
}

/* ---- networking messages ------------------------------------------------------------------- */

/* SteamNetworkingMessage_t carries the client's functions; Windows code calls msg->Release(). */
static uint64_t native_release;

static void WINAPI release_message(void *message)
{
	uint64_t arg = (uint64_t)(uintptr_t)message;
	if (native_release)
		call_native(native_release, &arg, 1);
}

static BOOL is_windows_code(uint64_t address)
{
	HMODULE module;
	return address && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                                     (const char *)(uintptr_t)address, &module);
}

static void message_to_windows(uint8_t *message)
{
	uint64_t *release = (uint64_t *)(message + STEAM_MESSAGE_RELEASE);
	if (!message || *release == (uint64_t)(uintptr_t)release_message)
		return;
	if (native_release && *release != native_release)
		WARN("a message with another release function %p\n", (void *)(uintptr_t)*release);
	native_release = *release;
	*release = (uint64_t)(uintptr_t)release_message;
}

static void message_to_native(uint8_t *message)
{
	uint64_t *release = (uint64_t *)(message + STEAM_MESSAGE_RELEASE);
	uint64_t *free_data = (uint64_t *)(message + STEAM_MESSAGE_FREE_DATA);
	if (!message)
		return;
	if (*release == (uint64_t)(uintptr_t)release_message)
		*release = native_release;
	if (is_windows_code(*free_data))
		*free_data = bind_hook(DEFERRED_FREE_MESSAGE, *free_data);
}

/* ---- interface proxies --------------------------------------------------------------------- */

struct proxy {
	void *const *vtbl;
	uint64_t native;
	const struct steam_interface *iface;
	BOOL is_client; /* ISteamClient: its methods return other interfaces */
	char version[64];
};

static void *stub_table[STEAM_STUB_COUNT];
static struct proxy *proxies[1024];
static unsigned proxy_count;
static SRWLOCK proxy_lock = SRWLOCK_INIT;

static struct proxy *wrap(uint64_t native, const char *version)
{
	struct proxy *proxy = NULL;
	if (!native)
		return NULL;
	AcquireSRWLockExclusive(&proxy_lock);
	for (unsigned i = 0; i < proxy_count && !proxy; i++)
		if (proxies[i]->native == native && !strcmp(proxies[i]->version, version))
			proxy = proxies[i];
	if (!proxy && proxy_count < ARRAY_SIZE(proxies)) {
		proxy = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*proxy));
		if (proxy) {
			proxy->vtbl = stub_table;
			proxy->native = native;
			proxy->iface = steam_find_interface(version);
			proxy->is_client = !strncmp(version, "SteamClient", 11);
			lstrcpynA(proxy->version, version, sizeof(proxy->version));
			proxies[proxy_count++] = proxy;
			if (proxy->iface)
				TRACE("interface %s (%u methods)\n", version, proxy->iface->method_count);
			else
				FIXME("interface %s is not described; its methods are forwarded as integers\n", version);
		}
	}
	ReleaseSRWLockExclusive(&proxy_lock);
	return proxy;
}

/* An argument that may be a pointer to an interface version string. */
static const char *version_argument(uint64_t value, char *out)
{
	MEMORY_BASIC_INFORMATION info;
	const char *text = (const char *)(uintptr_t)value;
	size_t n;
	if (value < 0x10000 || !VirtualQuery(text, &info, sizeof(info)) || info.State != MEM_COMMIT ||
	    !(info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
		return NULL;
	n = (const char *)info.BaseAddress + info.RegionSize - text;
	if (n > 63)
		n = 63;
	memcpy(out, text, n);
	out[n] = 0;
	return steam_is_version_string(out) ? out : NULL;
}

uint64_t steam_dispatch(struct steam_frame *frame)
{
	struct proxy *proxy = (struct proxy *)(uintptr_t)frame->gpr[0];
	const struct steam_method *method = steam_find_method(proxy->iface, frame->index);
	uint64_t fn, result;
	struct sysv_call call;

	if (proxy->iface && frame->index >= proxy->iface->method_count)
		WARN("%s: method %u is past the %u it has\n", proxy->version, frame->index, proxy->iface->method_count);
	fn = ((uint64_t *)(uintptr_t)*(uint64_t *)(uintptr_t)proxy->native)[frame->index];

	if (method && method->fnarg != 0xff) {
		uint64_t *arg = &frame->gpr[steam_param_position(method, method->fnarg)];
		switch (method->special) {
		case FN_ALWAYS_ONE: *arg = *arg ? native_always_one : 0; break;
		case FN_NOTHING: *arg = *arg ? native_nothing : 0; break;
		case FN_INT_STRING: *arg = bind_hook(DEFERRED_INT_STRING, *arg); break;
		default:
			if (*arg)
				FIXME("%s %s: a Windows function the bridge cannot hand over; passing NULL\n", proxy->version,
				      method->name);
			*arg = 0;
			break;
		}
	}
	if (method && method->special == MSG_SEND) {
		uint32_t count = (uint32_t)frame->gpr[steam_param_position(method, 0)];
		uint8_t **messages = (uint8_t **)(uintptr_t)frame->gpr[steam_param_position(method, 1)];
		for (uint32_t i = 0; messages && i < count; i++)
			message_to_native(messages[i]);
	}

	if (!steam_plan_call(method, fn, proxy->native, frame, &call)) {
		FIXME("%s %s (method %u) passes or returns a structure by value, or a callback object; not bridged\n", proxy->version,
		      method ? method->name : "?", frame->index);
		return 0;
	}
	STEAM_CALL(invoke, &call);
	result = steam_finish_call(method, &call, frame);

	if (method && method->special == MSG_RECEIVE) {
		uint8_t **messages = (uint8_t **)(uintptr_t)frame->gpr[steam_param_position(method, 1)];
		for (int i = 0; messages && i < (int)result; i++)
			message_to_windows(messages[i]);
	} else if (method && method->special == MSG_ALLOCATE && result) {
		message_to_windows((uint8_t *)(uintptr_t)result);
	}
	if (proxy->is_client && result) {
		/* GetISteamUser(user, pipe, version) and its kind: the result is that interface. */
		char buffer[64];
		const char *version = NULL;
		for (unsigned position = 1; position <= 3 && !version; position++)
			version = version_argument(frame->gpr[position], buffer);
		if (version)
			result = (uint64_t)(uintptr_t)wrap(result, version);
	}
	TRACE("%s method %u%s%s -> %#I64x\n", proxy->version, frame->index, method ? " " : "",
	      method ? method->name : "", result);
	run_deferred();
	return result;
}

/* ---- exports ------------------------------------------------------------------------------ */

void *CDECL CreateInterface(const char *version, int *code)
{
	static uint64_t create;
	uint64_t args[2] = {(uint64_t)(uintptr_t)version, (uint64_t)(uintptr_t)code};
	if (!create)
		create = native_symbol("CreateInterface");
	if (!create || !version)
		return NULL;
	return wrap(call_native(create, args, 2), version);
}

/* Steam's flat exports take integers and pointers only. */
#define FLAT(name)                                                                                                 \
	uint64_t CDECL name(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g,      \
	                    uint64_t h)                                                                              \
	{                                                                                                            \
		static uint64_t fn;                                                                                      \
		uint64_t args[8] = {a, b, c, d, e, f, g, h}, result;                                                     \
		if (!fn)                                                                                                 \
			fn = native_symbol(#name);                                                                           \
		if (!fn) {                                                                                               \
			FIXME(#name ": not in the Mac Steam client\n");                                                      \
			return 0;                                                                                            \
		}                                                                                                        \
		result = call_native(fn, args, 8);                                                                       \
		run_deferred();                                                                                          \
		return result;                                                                                           \
	}
FLAT(Steam_BConnected)
FLAT(Steam_BLoggedOn)
FLAT(Steam_BReleaseSteamPipe)
FLAT(Steam_ConnectToGlobalUser)
FLAT(Steam_CreateGlobalUser)
FLAT(Steam_CreateLocalUser)
FLAT(Steam_CreateSteamPipe)
FLAT(Steam_FreeLastCallback)
FLAT(Steam_GSBLoggedOn)
FLAT(Steam_GSBSecure)
FLAT(Steam_GSGetSteam2GetEncryptionKeyToSendToNewClient)
FLAT(Steam_GSGetSteamID)
FLAT(Steam_GSLogOff)
FLAT(Steam_GSLogOn)
FLAT(Steam_GSRemoveUserConnect)
FLAT(Steam_GSSendSteam2UserConnect)
FLAT(Steam_GSSendSteam3UserConnect)
FLAT(Steam_GSSendUserDisconnect)
FLAT(Steam_GSSendUserStatusResponse)
FLAT(Steam_GSSetServerType)
FLAT(Steam_GSSetSpawnCount)
FLAT(Steam_GSUpdateStatus)
FLAT(Steam_GetAPICallResult)
FLAT(Steam_GetGSHandle)
FLAT(Steam_InitiateGameConnection)
FLAT(Steam_LogOff)
FLAT(Steam_LogOn)
FLAT(Steam_ReleaseThreadLocalMemory)
FLAT(Steam_ReleaseUser)
FLAT(Steam_SetLocalIPBinding)
FLAT(Steam_TerminateGameConnection)

/* CallbackMsg_t, and the callbacks whose layout differs: the Mac client packs callback
 * structures to 4 bytes, Windows to 8, so an 8-byte member after an odd number of 4-byte
 * ones moves. */
struct callback_msg {
	int32_t user;
	int32_t id;
	uint8_t *data;
	int32_t size;
};

static DWORD converted_slot = TLS_OUT_OF_INDEXES;

BOOL CDECL Steam_BGetCallback(int32_t pipe, struct callback_msg *message)
{
	static uint64_t fn;
	uint64_t args[2] = {(uint64_t)pipe, (uint64_t)(uintptr_t)message};
	run_deferred();
	if (!fn)
		fn = native_symbol("Steam_BGetCallback");
	if (!fn || !(BOOL)call_native(fn, args, 2))
		return FALSE;
	/* SteamNetConnectionStatusChangedCallback_t: m_hConn, m_info (8-aligned on Windows), m_eOldState. */
	if (message->id == 1221 && message->size >= 4 + 696 && message->size <= 2048) {
		uint8_t *converted = TlsGetValue(converted_slot);
		if (!converted) {
			converted = HeapAlloc(GetProcessHeap(), 0, 712);
			TlsSetValue(converted_slot, converted);
		}
		if (converted) {
			memset(converted, 0, 712);
			memcpy(converted, message->data, 4);
			memcpy(converted + 8, message->data + 4, 696);
			memcpy(converted + 704, message->data + 700, 4);
			message->data = converted;
			message->size = 712;
		}
	}
	TRACE("callback %d (%d bytes)\n", message->id, message->size);
	return TRUE;
}

BOOL CDECL Steam_IsKnownInterface(const char *version)
{
	struct known_interface_params params = {.version = version};
	if (!version || !ensure_loaded() || STEAM_CALL(known_interface, &params))
		return FALSE;
	TRACE("%s: %s\n", version, params.known ? "known" : "unknown");
	return params.known;
}

void CDECL Steam_NotifyMissingInterface(int32_t pipe, const char *version)
{
	(void)pipe;
	FIXME("the Mac Steam client lacks interface %s\n", debugstr_a(version));
}

/* Crash reporting stays with Wine: the client's own reporter would take its signals. */
uint64_t CDECL Breakpad_Ignored(void)
{
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	(void)reserved;
	if (reason != DLL_PROCESS_ATTACH)
		return TRUE;
	DisableThreadLibraryCalls(instance);
	if (__wine_init_unix_call())
		return FALSE;
	for (unsigned i = 0; i < STEAM_STUB_COUNT; i++)
		stub_table[i] = (void *)(steam_stubs + i * STEAM_STUB_SIZE);
	running_deferred = TlsAlloc();
	converted_slot = TlsAlloc();
	return TRUE;
}
