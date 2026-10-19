/* Copyright 2026, Nick Kennedy
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Native half of the Wine Steam bridge: loads the Mac Steam client's
 * steamclient.dylib (its x86-64 slice, in this Rosetta process) and makes the
 * System V calls the Windows half describes. It never calls Windows code:
 * hooks the client would call are queued for the Windows half. */
#define _GNU_SOURCE       /* sigaction, memmem, dladdr */
#define _DARWIN_C_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

#ifdef MWXR_STEAM_BRIDGE_TEST
typedef long NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)(void *args);
#define STATUS_SUCCESS 0
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)
#else
#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winternl.h>
#include "wine/unixlib.h"
#endif

#include "unixlib.h"

_Static_assert(offsetof(struct sysv_call, gpr) == 8, "sysv_invoke.S offsets");
_Static_assert(offsetof(struct sysv_call, xmm) == 56, "sysv_invoke.S offsets");
_Static_assert(offsetof(struct sysv_call, stack) == 120, "sysv_invoke.S offsets");
_Static_assert(offsetof(struct sysv_call, nstack) == 248, "sysv_invoke.S offsets");
_Static_assert(offsetof(struct sysv_call, ret_rax) == 256, "sysv_invoke.S offsets");
_Static_assert(offsetof(struct sysv_call, ret_xmm0) == 264, "sysv_invoke.S offsets");

void sysv_invoke(struct sysv_call *call);

static void *client;
/* Calls queued for the Windows half. It reads this directly, so an empty queue costs no unix call. */
static volatile uint32_t pending;

/* ---- loading ------------------------------------------------------------------------------ */

/* The client installs crash handlers (signals, and on macOS the task's exception ports).
 * Wine depends on its own, so whatever loading changes is put back. */
struct handlers {
	struct sigaction actions[32];
#ifdef __APPLE__
	exception_mask_t masks[EXC_TYPES_COUNT];
	mach_port_t ports[EXC_TYPES_COUNT];
	exception_behavior_t behaviors[EXC_TYPES_COUNT];
	thread_state_flavor_t flavors[EXC_TYPES_COUNT];
	mach_msg_type_number_t count;
#endif
};

static void save_handlers(struct handlers *h)
{
	for (int i = 1; i < 32; i++)
		sigaction(i, NULL, &h->actions[i]);
#ifdef __APPLE__
	h->count = EXC_TYPES_COUNT;
	if (task_get_exception_ports(mach_task_self(), EXC_MASK_ALL, h->masks, &h->count, h->ports, h->behaviors,
	                             h->flavors) != KERN_SUCCESS)
		h->count = 0;
#endif
}

static void restore_handlers(const struct handlers *h)
{
	for (int i = 1; i < 32; i++) {
		struct sigaction now;
		if (i == SIGKILL || i == SIGSTOP)
			continue;
		sigaction(i, NULL, &now);
		if (now.sa_handler != h->actions[i].sa_handler) {
			fprintf(stderr, "mwxr steam bridge: steamclient replaced the handler of signal %d; restored\n", i);
			sigaction(i, &h->actions[i], NULL);
		}
	}
#ifdef __APPLE__
	for (mach_msg_type_number_t i = 0; i < h->count; i++)
		task_set_exception_ports(mach_task_self(), h->masks[i], h->ports[i], h->behaviors[i], h->flavors[i]);
#endif
}

static uint32_t always_one(int32_t callback_id)
{
	(void)callback_id;
	return 1;
}

static void nothing(void)
{
}

static NTSTATUS init(void *args)
{
	struct init_params *params = args;
	char path[1024];
	struct handlers saved;

	/* The Windows environment is not the native one: the client reads the app id natively, as
	 * Proton's lsteamclient also arranges. */
	if (params->app_id[0])
		setenv("SteamAppId", params->app_id, 1);
	params->always_one = (uint64_t)(uintptr_t)always_one;
	params->nothing = (uint64_t)(uintptr_t)nothing;
	params->pending = (uint64_t)(uintptr_t)&pending;
	if (client) {
		params->ok = 1;
		return STATUS_SUCCESS;
	}
	if (params->path[0])
		snprintf(path, sizeof(path), "%s", params->path);
	else
		snprintf(path, sizeof(path),
		         "%s/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib",
		         getenv("HOME") ? getenv("HOME") : "");
	save_handlers(&saved);
	client = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	restore_handlers(&saved);
	if (!client) {
		snprintf(params->error, sizeof(params->error), "%s", dlerror());
		params->ok = 0;
		return STATUS_UNSUCCESSFUL;
	}
	params->ok = 1;
	return STATUS_SUCCESS;
}

static NTSTATUS lookup(void *args)
{
	struct lookup_params *params = args;
	params->fn = client ? (uint64_t)(uintptr_t)dlsym(client, params->name) : 0;
	return params->fn ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static NTSTATUS invoke(void *args)
{
	struct sysv_call *call = args;
	struct handlers saved;
	/* The first calls start the client's threads and may install handlers again. */
	static int calls;
	int watch = __atomic_fetch_add(&calls, 1, __ATOMIC_RELAXED) < 64;
	if (watch)
		save_handlers(&saved);
	sysv_invoke(call);
	if (watch)
		restore_handlers(&saved);
	return STATUS_SUCCESS;
}

/* The Mac client exports neither Steam_IsKnownInterface nor Steam_NotifyMissingInterface.
 * A version is known when the client's binary carries the whole string. */
static const char *client_image;
static size_t client_image_size;

static void map_client_image(void)
{
	Dl_info info;
	struct stat st;
	void *create = dlsym(client, "CreateInterface");
	int fd = create && dladdr(create, &info) ? open(info.dli_fname, O_RDONLY) : -1;
	if (fd < 0)
		return;
	if (fstat(fd, &st) == 0 && st.st_size > 0) {
		void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (map != MAP_FAILED) {
			client_image = map;
			client_image_size = (size_t)st.st_size;
		}
	}
	close(fd);
}

static NTSTATUS known_interface(void *args)
{
	struct known_interface_params *params = args;
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	size_t n = strlen(params->version) + 1; /* with its NUL: the whole string */

	params->known = 0;
	if (!client)
		return STATUS_UNSUCCESSFUL;
	pthread_once(&once, map_client_image);
	if (!client_image)
		return STATUS_UNSUCCESSFUL;
	for (const char *p = client_image; p < client_image + client_image_size; p++) {
		p = memmem(p, client_image_size - (size_t)(p - client_image), params->version, n);
		if (!p)
			break;
		if (p == client_image || !p[-1]) { /* not the tail of a longer string */
			params->known = 1;
			break;
		}
	}
	return STATUS_SUCCESS;
}

/* ---- calls the client makes into the game, queued -------------------------------------- */

struct queued {
	struct queued *next;
	struct deferred_call call;
};
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static struct queued *queue_head, **queue_tail = &queue_head;


static void enqueue(const struct deferred_call *call)
{
	struct queued *entry = malloc(sizeof(*entry));
	if (!entry)
		return;
	entry->next = NULL;
	entry->call = *call;
	pthread_mutex_lock(&queue_lock);
	*queue_tail = entry;
	queue_tail = &entry->next;
	__atomic_add_fetch(&pending, 1, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&queue_lock);
}

static NTSTATUS next_deferred(void *args)
{
	struct deferred_call *out = args;
	struct queued *entry;
	pthread_mutex_lock(&queue_lock);
	entry = queue_head;
	if (entry) {
		queue_head = entry->next;
		if (!queue_head)
			queue_tail = &queue_head;
		__atomic_sub_fetch(&pending, 1, __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&queue_lock);
	if (!entry) {
		out->kind = 0;
		return STATUS_SUCCESS;
	}
	*out = entry->call;
	free(entry);
	return STATUS_SUCCESS;
}

#define SLOTS 16
static uint64_t hook_targets[SLOTS], free_targets[SLOTS];

static void queue_int_string(int slot, int32_t value, const char *text)
{
	struct deferred_call call = {.kind = DEFERRED_INT_STRING, .value = value, .windows_fn = hook_targets[slot]};
	snprintf(call.text, sizeof(call.text), "%s", text ? text : "");
	enqueue(&call);
}

static void queue_free_message(int slot, const void *message)
{
	struct deferred_call call = {.kind = DEFERRED_FREE_MESSAGE, .windows_fn = free_targets[slot]};
	if (!message)
		return;
	memcpy(call.message, message, sizeof(call.message));
	enqueue(&call);
}

#define HOOK(n)                                                                                                    \
	static void hook##n(int32_t value, const char *text) { queue_int_string(n, value, text); }                     \
	static void free##n(void *message) { queue_free_message(n, message); }
HOOK(0) HOOK(1) HOOK(2) HOOK(3) HOOK(4) HOOK(5) HOOK(6) HOOK(7)
HOOK(8) HOOK(9) HOOK(10) HOOK(11) HOOK(12) HOOK(13) HOOK(14) HOOK(15)
static void (*const hooks[SLOTS])(int32_t, const char *) = {
	hook0, hook1, hook2, hook3, hook4, hook5, hook6, hook7, hook8, hook9, hook10, hook11, hook12, hook13, hook14, hook15,
};
static void (*const frees[SLOTS])(void *) = {
	free0, free1, free2, free3, free4, free5, free6, free7, free8, free9, free10, free11, free12, free13, free14, free15,
};

static NTSTATUS bind_hook(void *args)
{
	struct bind_hook_params *params = args;
	uint64_t *targets = params->kind == DEFERRED_FREE_MESSAGE ? free_targets : hook_targets;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int slot;

	params->native_fn = 0;
	if (!params->windows_fn)
		return STATUS_SUCCESS;
	pthread_mutex_lock(&lock);
	for (slot = 0; slot < SLOTS && targets[slot] && targets[slot] != params->windows_fn; slot++)
		;
	if (slot < SLOTS)
		targets[slot] = params->windows_fn;
	pthread_mutex_unlock(&lock);
	if (slot == SLOTS)
		return STATUS_UNSUCCESSFUL;
	params->native_fn = params->kind == DEFERRED_FREE_MESSAGE ? (uint64_t)(uintptr_t)frees[slot]
	                                                          : (uint64_t)(uintptr_t)hooks[slot];
	return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] = {
	init,
	lookup,
	invoke,
	bind_hook,
	next_deferred,
	known_interface,
};
_Static_assert(sizeof(__wine_unix_call_funcs) / sizeof(__wine_unix_call_funcs[0]) == unix_call_count,
               "one entry per enum steam_bridge_call");
