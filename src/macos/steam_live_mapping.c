// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Maps games to the Steam Play tool while Steam runs (docs/steam-play.md, Live mapping).
// Built into the compat gate library; does something only in steam_osx.
//
// Steam reads STEAM_COMPAT_TOOL_MAPPINGS once, as it starts, and hands each entry to
// CCompatManager::AddMapping(manager, appid, {tool, config, priority}). The same call
// made later maps a game at once: Steam logs "Mapping AppID N to tool ..." and updates the
// game, and an empty tool at the same priority unmaps it ("Unmapping AppID N"). So:
//
// - At load, AddMapping is found as the function called after the code that loads the
//   "STEAM_COMPAT_TOOL_MAPPINGS" string and that uses the "CCompatManager::AddMapping"
//   profiling label near its start. Exactly one such function must be found.
// - Its first four instructions (a plain prologue, checked) move to a trampoline that also
//   records its first argument, the compat manager, and the calling thread; Steam's own
//   calls at startup fill them in.
// - A watcher runs steam_play_games.py sync when the games file, appinfo.vdf or the
//   library cache change (a choice on the Compatibility page, a new game), into
//   mapped.live, and makes the differences from what Steam has with AddMapping, on the
//   thread Steam called it from: its engine thread, when that next waits in kevent
//   between frames. Then mapped.live becomes mapped, which is what the page reads as
//   "what Steam has".
//
// If anything is missing, nothing is hooked and changes wait for a restart, as before.
// MWXR_STEAM_LIVE_MAPPING=0 turns this off.

#include <dispatch/dispatch.h>
#include <crt_externs.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__aarch64__)
#include <libkern/OSCacheControl.h>
#endif

void mwxr_gate_log(const char *fmt, ...);

#define TOOL_NAME "macos-wine-xr-proton"
#define PRIORITY 250 // as Steam Play.app's STEAM_COMPAT_TOOL_MAPPINGS
#define POLL_SECONDS 5

#if defined(__aarch64__)

uintptr_t mwxr_adrp_add_target(const uint32_t *w, size_t i, size_t first);
int mwxr_write_code(uint32_t *addr, const uint32_t *insns, size_t count);

// AddMapping's third argument, as Steam's startup code builds it: two CUtlStrings (a
// char pointer each; NULL reads as "") and the priority. AddMapping copies the strings.
struct mapping {
	const char *tool;
	const char *config;
	int32_t priority;
};
typedef void (*add_mapping_fn)(void *manager, uint32_t appid, const struct mapping *m);

// Written by the trampoline at each call of AddMapping.
static struct {
	void *volatile manager;
	volatile uint64_t thread; // TPIDRRO_EL0 of the calling thread
} captured;
static add_mapping_fn add_mapping;

static int64_t sign_extend(uint64_t value, int bits)
{
	uint64_t m = 1ull << (bits - 1);
	return (int64_t)((value ^ m) - m);
}

// The address of the C string exactly equal to s, if it appears once.
static uintptr_t unique_cstring(const struct mach_header_64 *mh, const char *s)
{
	unsigned long size = 0;
	const char *cs = (const char *)getsectiondata(mh, "__TEXT", "__cstring", &size);
	size_t len = strlen(s) + 1;
	uintptr_t found = 0;
	int count = 0;
	for (const char *p = cs; cs && (p = memmem(p, size - (unsigned long)(p - cs), s, len)) != NULL; p += len) {
		if (p == cs || p[-1] == '\0') {
			found = (uintptr_t)p;
			count++;
		}
	}
	return count == 1 ? found : 0;
}

// Instructions that do not depend on where they run, so a trampoline can run them: the
// prologue AddMapping starts with (sub sp; stp ...), and nothing PC-relative.
static int relocatable(uint32_t insn)
{
	return (insn & 0x7C000000u) != 0x14000000u     // b, bl
	    && (insn & 0xFE000000u) != 0x54000000u     // b.cond
	    && (insn & 0x7E000000u) != 0x34000000u     // cbz, cbnz
	    && (insn & 0x7E000000u) != 0x36000000u     // tbz, tbnz
	    && (insn & 0x1F000000u) != 0x10000000u     // adr, adrp
	    && (insn & 0x3B000000u) != 0x18000000u;    // ldr (literal)
}

static uint32_t *find_add_mapping(const struct mach_header_64 *mh)
{
	uintptr_t env = unique_cstring(mh, "STEAM_COMPAT_TOOL_MAPPINGS");
	uintptr_t label = unique_cstring(mh, "CCompatManager::AddMapping");
	if (!env || !label) {
		mwxr_gate_log("live mapping: %s string not found once", env ? "AddMapping label" : "STEAM_COMPAT_TOOL_MAPPINGS");
		return NULL;
	}
	unsigned long size = 0;
	uint32_t *w = (uint32_t *)getsectiondata(mh, "__TEXT", "__text", &size);
	size_t n = size / 4;
	uintptr_t labels[4];
	int nlabels = 0;
	for (size_t i = 0; w && i < n && nlabels < 4; i++) {
		if (mwxr_adrp_add_target(w, i, 0) == label)
			labels[nlabels++] = (uintptr_t)&w[i];
	}
	uint32_t *found = NULL;
	int distinct = 0;
	for (size_t i = 0; w && i < n; i++) {
		if ((w[i] & 0x1F) != 0 || mwxr_adrp_add_target(w, i, 0) != env) // getenv(x0)
			continue;
		for (size_t j = i + 1; j < n && j <= i + 128; j++) {
			if ((w[j] & 0xFC000000u) != 0x94000000u) // bl
				continue;
			uintptr_t target = (uintptr_t)&w[j] + (uintptr_t)(sign_extend(w[j] & 0x3FFFFFFu, 26) * 4);
			for (int l = 0; l < nlabels; l++) {
				if (target <= labels[l] && labels[l] - target < 0x100 && (uint32_t *)target != found) {
					found = (uint32_t *)target;
					distinct++;
				}
			}
		}
	}
	if (distinct != 1) {
		mwxr_gate_log("live mapping: found %d AddMapping candidates, expected 1", distinct);
		return NULL;
	}
	if ((found[0] & 0xFF8003FFu) != 0xD10003FFu) { // sub sp, sp, #imm
		mwxr_gate_log("live mapping: AddMapping does not start with sub sp (0x%08x)", found[0]);
		return NULL;
	}
	for (int k = 0; k < 4; k++) {
		if (!relocatable(found[k])) {
			mwxr_gate_log("live mapping: AddMapping instruction %d is PC-relative (0x%08x)", k, found[k]);
			return NULL;
		}
	}
	mwxr_gate_log("live mapping: AddMapping at steamclient+0x%lx", (unsigned long)((uintptr_t)found - (uintptr_t)mh));
	return found;
}

// fn's first four instructions become "ldr x16, #8; br x16; .quad trampoline". The
// trampoline stores x0 and the thread, runs those four instructions and returns to fn+16.
static int hook(uint32_t *fn)
{
	uint32_t *t = mmap(NULL, vm_page_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
	if (t == MAP_FAILED) {
		mwxr_gate_log("live mapping: no executable page for the trampoline (%d)", errno);
		return 0;
	}
	pthread_jit_write_protect_np(0);
	t[0] = 0x58000000u | (10u << 5) | 16; // ldr x16, t[10] (&captured)
	t[1] = 0xF9000200u;                    // str x0, [x16]
	t[2] = 0xD53BD071u;                    // mrs x17, tpidrro_el0
	t[3] = 0xF9000611u;                    // str x17, [x16, #8]
	memcpy(&t[4], fn, 4 * sizeof(*fn));     // fn's first four instructions
	t[8] = 0x58000000u | (4u << 5) | 16;   // ldr x16, t[12] (fn + 16)
	t[9] = 0xD61F0200u;                    // br x16
	uint64_t lits[2] = {(uint64_t)(uintptr_t)&captured, (uint64_t)(uintptr_t)(fn + 4)};
	memcpy(&t[10], lits, sizeof(lits));
	pthread_jit_write_protect_np(1);
	sys_icache_invalidate(t, 14 * sizeof(*t));
	uint64_t tramp = (uint64_t)(uintptr_t)t;
	uint32_t patch[4] = {0x58000050u, 0xD61F0200u, (uint32_t)tramp, (uint32_t)(tramp >> 32)};
	return mwxr_write_code(fn, patch, 4);
}

static uint64_t thread_register(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, tpidrro_el0" : "=r"(v));
	return v;
}

// --- The watcher ---------------------------------------------------------------------

struct ids {
	uint32_t *v;
	size_t n, cap;
};

static int ids_has(const struct ids *s, uint32_t id)
{
	for (size_t i = 0; i < s->n; i++)
		if (s->v[i] == id)
			return 1;
	return 0;
}

static void ids_add(struct ids *s, uint32_t id)
{
	if (!id || ids_has(s, id))
		return;
	if (s->n == s->cap) {
		size_t cap = s->cap ? s->cap * 2 : 256;
		uint32_t *v = realloc(s->v, cap * sizeof(*v));
		if (!v)
			return;
		s->v = v;
		s->cap = cap;
	}
	s->v[s->n++] = id;
}

static char payload[PATH_MAX], steam_dir[PATH_MAX], live_file[PATH_MAX];
static struct ids applied; // what Steam has mapped to the tool at priority 250
static dispatch_queue_t queue;
static atomic_int disabled;

static void path_in(char *out, size_t len, const char *dir, const char *name)
{
	snprintf(out, len, "%s/%s", dir, name);
}

// The app IDs a mapped file lists (lines that start with one).
static int read_mapped(const char *path, struct ids *out)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		char *end = NULL;
		unsigned long id = strtoul(line, &end, 10);
		if (end != line && id && id <= UINT32_MAX)
			ids_add(out, (uint32_t)id);
	}
	fclose(f);
	return 1;
}

// Steam's mappings from Steam Play.app: "APPID 250 macos-wine-xr-proton,...".
static void read_environment(struct ids *out)
{
	const char *env = getenv("STEAM_COMPAT_TOOL_MAPPINGS");
	for (const char *p = env; p && *p;) {
		char *end = NULL;
		unsigned long id = strtoul(p, &end, 10);
		const char *next = strchr(p, ',');
		size_t len = next ? (size_t)(next - p) : strlen(p);
		if (end != p && id && memmem(p, len, " " TOOL_NAME, strlen(" " TOOL_NAME)))
			ids_add(out, (uint32_t)id);
		p = next ? next + 1 : NULL;
	}
}

static int run_sync(const char *output)
{
	char script[PATH_MAX];
	path_in(script, sizeof(script), payload, "steam_play_games.py");
	char *args[] = {"/usr/bin/python3", "-I", script, "sync", "--payload", payload, "--output", (char *)output, NULL};
	pid_t pid;
	int status = 0;
	if (posix_spawn(&pid, args[0], NULL, NULL, args, *_NSGetEnviron()) != 0)
		return 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

struct change {
	struct ids add, remove;
	int done;
};

// TPIDRRO_EL0 values of one thread (the low bits are not part of the thread's address).
static int same_thread(uint64_t a, uint64_t b)
{
	return (a & ~7ull) == (b & ~7ull);
}

// The name of the thread with this TPIDRRO_EL0 (its first TSD slot is its pthread_t).
static void thread_name(uint64_t reg, char *out, size_t len)
{
	snprintf(out, len, "?");
	pthread_t t = reg ? *(pthread_t *)(uintptr_t)(reg & ~7ull) : NULL;
	if (t && (pthread_getname_np(t, out, len) != 0 || !out[0]))
		snprintf(out, len, "%s", pthread_main_np() && t == pthread_self() ? "main" : "unnamed");
}

static uint64_t main_thread; // set by the constructor below, which runs on the main thread

// On Steam's thread: the AddMapping calls. Returns 0, changing nothing, when this is not
// the thread Steam called AddMapping from.
static int apply_on_steam_thread(struct change *c)
{
	if (atomic_load(&disabled) || !same_thread(thread_register(), captured.thread))
		return 0;
	struct mapping add = {TOOL_NAME, NULL, PRIORITY}, remove = {NULL, NULL, PRIORITY};
	for (size_t i = 0; i < c->add.n; i++)
		add_mapping(captured.manager, c->add.v[i], &add);
	for (size_t i = 0; i < c->remove.n; i++)
		add_mapping(captured.manager, c->remove.v[i], &remove);
	return 1;
}

// The change waiting for Steam's thread, and the signal that it was run.
static struct change *_Atomic waiting;
static dispatch_semaphore_t taken;

static void run_waiting(void)
{
	struct change *c = atomic_exchange(&waiting, NULL);
	if (!c)
		return;
	c->done = apply_on_steam_thread(c);
	dispatch_semaphore_signal(taken);
}

// Steam's engine thread ("IPC:CSteamEngine") waits for its next frame in kevent, through
// steamclient's import of it: the change runs there, between frames.
static int mwxr_kevent(int kq, const struct kevent *changes, int nchanges, struct kevent *events, int nevents,
                       const struct timespec *timeout)
{
	if (atomic_load_explicit(&waiting, memory_order_relaxed) && same_thread(thread_register(), captured.thread))
		run_waiting();
	return kevent(kq, changes, nchanges, events, nevents, timeout);
}

__attribute__((used)) static const struct {
	const void *replacement, *original;
} live_mapping_interposers[] __attribute__((section("__DATA,__interpose"))) = {
	{(const void *)mwxr_kevent, (const void *)kevent},
};

// Runs the change on the thread Steam called AddMapping from, waiting up to 30 s. 0 if it
// did not run there; live mapping is then off until Steam restarts.
static int on_steam_thread(struct change *c)
{
	if (!taken)
		taken = dispatch_semaphore_create(0);
	c->done = 0;
	atomic_store(&waiting, c);
	if (same_thread(captured.thread, main_thread))
		dispatch_async(dispatch_get_main_queue(), ^{ run_waiting(); });
	if (dispatch_semaphore_wait(taken, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)) != 0) {
		if (atomic_exchange(&waiting, NULL) == c) {
			char name[64];
			thread_name(captured.thread, name, sizeof(name));
			mwxr_gate_log("live mapping: Steam's thread (%s) did not take the change in 30 s; off until Steam restarts", name);
			atomic_store(&disabled, 1);
			return 0;
		}
		dispatch_semaphore_wait(taken, DISPATCH_TIME_FOREVER); // it is running now
	}
	if (!c->done) {
		mwxr_gate_log("live mapping: the change ran on another thread; off until Steam restarts");
		atomic_store(&disabled, 1);
	}
	return c->done;
}

static void write_live_file(void)
{
	char text[32];
	snprintf(text, sizeof(text), "%d\n", getpid());
	int fd = open(live_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0) {
		(void)!write(fd, text, strlen(text));
		close(fd);
	}
}

static void remove_live_file(void)
{
	if (live_file[0])
		unlink(live_file);
}

static void sync_and_apply(void)
{
	char live[PATH_MAX], mapped[PATH_MAX];
	path_in(live, sizeof(live), payload, "mapped.live");
	path_in(mapped, sizeof(mapped), payload, "mapped");
	struct ids now = {0};
	if (!run_sync(live) || !read_mapped(live, &now)) {
		mwxr_gate_log("live mapping: steam_play_games.py sync failed; nothing changed");
		unlink(live);
		free(now.v);
		return;
	}
	struct change c = {0};
	for (size_t i = 0; i < now.n; i++)
		if (!ids_has(&applied, now.v[i]))
			ids_add(&c.add, now.v[i]);
	for (size_t i = 0; i < applied.n; i++)
		if (!ids_has(&now, applied.v[i]))
			ids_add(&c.remove, applied.v[i]);
	// The first sync checks Steam's thread even with nothing to change, before the page is
	// told (live-mapping) that changes apply at once.
	static int checked;
	int ok = 1;
	if (c.add.n || c.remove.n || !checked) {
		ok = checked = on_steam_thread(&c);
	}
	if (c.add.n || c.remove.n) {
		char list[512] = "";
		size_t used = 0;
		for (size_t i = 0; i < c.add.n && used < sizeof(list) - 16; i++)
			used += (size_t)snprintf(list + used, sizeof(list) - used, " +%u", c.add.v[i]);
		for (size_t i = 0; i < c.remove.n && used < sizeof(list) - 16; i++)
			used += (size_t)snprintf(list + used, sizeof(list) - used, " -%u", c.remove.v[i]);
		mwxr_gate_log("live mapping:%s%s", list, ok ? "" : " (not applied)");
	}
	if (ok) {
		free(applied.v);
		applied = now;
		rename(live, mapped);
		write_live_file();
	} else {
		free(now.v);
		unlink(live);
		remove_live_file();
	}
	free(c.add.v);
	free(c.remove.v);
}

// What a sync reads: the games file, Steam's app info and its library cache.
static int fingerprint(char *out, size_t len)
{
	static const char *const names[] = {"games", "appcache/appinfo.vdf", "appcache/librarycache"};
	size_t used = 0;
	for (int i = 0; i < 3; i++) {
		char path[PATH_MAX];
		path_in(path, sizeof(path), i == 0 ? payload : steam_dir, names[i]);
		struct stat st;
		if (stat(path, &st) != 0)
			memset(&st, 0, sizeof(st));
		used += (size_t)snprintf(out + used, len - used, "%llu.%ld.%ld.%lld;", (unsigned long long)st.st_ino,
		                         (long)st.st_mtimespec.tv_sec, st.st_mtimespec.tv_nsec, (long long)st.st_size);
	}
	return 1;
}

static void check(void)
{
	static char last[512];
	static int started;
	if (atomic_load(&disabled) || !captured.manager)
		return;
	char now[512];
	fingerprint(now, sizeof(now));
	if (started && strcmp(now, last) == 0)
		return;
	// A file still being written settles first: sync when two looks agree.
	usleep(300 * 1000);
	char again[512];
	fingerprint(again, sizeof(again));
	if (strcmp(now, again) != 0)
		return; // the next event or poll looks again
	if (!started) {
		char name[64];
		thread_name(captured.thread, name, sizeof(name));
		mwxr_gate_log("live mapping: on (compat manager %p, thread %s)", captured.manager, name);
	}
	started = 1;
	memcpy(last, now, sizeof(last));
	sync_and_apply();
}

static void start_watcher(void)
{
	snprintf(payload, sizeof(payload), "%s/Library/Application Support/macos-wine-xr/steam-play", getenv("HOME"));
	snprintf(steam_dir, sizeof(steam_dir), "%s/Library/Application Support/Steam", getenv("HOME"));
	path_in(live_file, sizeof(live_file), payload, "live-mapping");
	remove_live_file();
	atexit(remove_live_file);
	read_environment(&applied);
	queue = dispatch_queue_create("mwxr.live-mapping", DISPATCH_QUEUE_SERIAL);
	// The page writes games by renaming a file into the payload folder.
	int fd = open(payload, O_EVTONLY | O_CLOEXEC);
	if (fd >= 0) {
		dispatch_source_t dir = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, (uintptr_t)fd,
		                                               DISPATCH_VNODE_WRITE, queue);
		dispatch_source_set_event_handler(dir, ^{ check(); });
		dispatch_source_set_cancel_handler(dir, ^{ close(fd); });
		dispatch_resume(dir);
	}
	// Steam's caches change in place, and less often: a poll.
	dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, POLL_SECONDS * NSEC_PER_SEC),
	                          POLL_SECONDS * NSEC_PER_SEC, NSEC_PER_SEC);
	dispatch_source_set_event_handler(timer, ^{ check(); });
	dispatch_resume(timer);
}

__attribute__((constructor)) static void record_main_thread(void)
{
	main_thread = thread_register();
}

int mwxr_live_mapping_attach(const void *mh, int apply)
{
	const char *off = getenv("MWXR_STEAM_LIVE_MAPPING");
	if (apply && off && strcmp(off, "0") == 0) {
		mwxr_gate_log("live mapping: off (MWXR_STEAM_LIVE_MAPPING=0)");
		return 0;
	}
	uint32_t *fn = find_add_mapping(mh);
	if (!fn || !apply)
		return fn != NULL;
	add_mapping = (add_mapping_fn)(void *)fn; // called through the hook, which records again
	if (!hook(fn))
		return 0;
	start_watcher();
	return 1;
}

#else

int mwxr_live_mapping_attach(const void *mh, int apply)
{
	(void)mh;
	(void)apply;
	return 0;
}

#endif
