// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Turns on Steam Play in a running macOS Steam client, in memory only
// (docs/steam-play.md).
//
// Steam's compatibility manager decides once, as steamclient.dylib starts, whether
// compatibility tools are used at all, by comparing the host OS with "linux":
//
//     bl     V_strnicmp            ; (host os, "linux", INT_MAX)
//     cmp    w0, #0
//     cset   wN, eq                ; -> mov wN, #1
//     strb   wN, [xM, #field]
//
// This library, loaded into steam_osx with DYLD_INSERT_LIBRARIES by the Steam Play
// launcher, waits for steamclient.dylib to load and rewrites that cset as "mov wN, #1".
// The site is found from the code that loads the "linux" string, not from the field's
// offset, which changes between Steam builds (0x7b0 in 1785187029, 0x810 in 1793056502).
// It patches only when there is exactly one such site. Nothing on disk is changed.
//
// Children of Steam inherit DYLD_INSERT_LIBRARIES, so it does nothing outside steam_osx.
// steam_ui_patch.c, in the same library, shows the Compatibility page in a game's
// Properties, in Steam's web helper; steam_live_mapping.c maps games chosen or bought while
// Steam runs.
// Log: ~/Library/Logs/macos-wine-xr/steam-compat-gate.log (MWXR_STEAM_COMPAT_GATE_LOG).

#include <dlfcn.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__aarch64__)
#include <libkern/OSCacheControl.h>
#endif

#define TARGET_IMAGE "/steamclient.dylib"
#define HOST_PROCESS "steam_osx"

// Shared with steam_ui_patch.c, which is built into the same library.
void mwxr_gate_log(const char *fmt, ...)
{
	char path[PATH_MAX];
	const char *override = getenv("MWXR_STEAM_COMPAT_GATE_LOG");
	const char *home = getenv("HOME");
	if (override && *override) {
		snprintf(path, sizeof(path), "%s", override);
	} else {
		if (!home || !*home)
			return;
		snprintf(path, sizeof(path), "%s/Library/Logs/macos-wine-xr", home);
		mkdir(path, 0700);
		strlcat(path, "/steam-compat-gate.log", sizeof(path));
	}
	int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return;
	FILE *f = fdopen(fd, "a");
	if (!f) {
		close(fd);
		return;
	}
	fprintf(f, "[%d] ", getpid());
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

static int in_steam_client(void)
{
	char path[PATH_MAX];
	uint32_t size = sizeof(path);
	if (_NSGetExecutablePath(path, &size) != 0)
		return 0;
	return strcmp(basename(path), HOST_PROCESS) == 0;
}

#if defined(__aarch64__)

#define MAX_STRINGS 8

// Addresses of every "linux\0" in the image's C strings.
static int find_strings(const struct mach_header_64 *mh, uintptr_t *found)
{
	unsigned long size = 0;
	const char *s = (const char *)getsectiondata(mh, "__TEXT", "__cstring", &size);
	int n = 0;
	for (unsigned long i = 0; s && i + 6 <= size && n < MAX_STRINGS; i++) {
		if ((i == 0 || s[i - 1] == '\0') && memcmp(s + i, "linux", 6) == 0)
			found[n++] = (uintptr_t)(s + i);
	}
	return n;
}

static int64_t sign_extend(uint64_t value, int bits)
{
	uint64_t m = 1ull << (bits - 1);
	return (int64_t)((value ^ m) - m);
}

// The address "adrp xR, page; add xD, xR, #lo" puts in xD, or 0 if w[i] is not such an
// add with its adrp in the eight instructions before it. Shared with steam_live_mapping.c.
uintptr_t mwxr_adrp_add_target(const uint32_t *w, size_t i, size_t first)
{
	uint32_t add = w[i];
	if ((add & 0xFF000000u) != 0x91000000u) // add (immediate), 64-bit
		return 0;
	uint32_t rn = (add >> 5) & 0x1F;
	uint64_t lo = ((add >> 10) & 0xFFF) << (((add >> 22) & 1) ? 12 : 0);
	for (size_t k = 1; k <= 8 && i >= first + k; k++) {
		uint32_t adrp = w[i - k];
		if ((adrp & 0x9F000000u) != 0x90000000u || (adrp & 0x1F) != rn)
			continue;
		uint64_t imm = ((uint64_t)((adrp >> 5) & 0x7FFFF) << 2) | ((adrp >> 29) & 3);
		uintptr_t pc = (uintptr_t)&w[i - k];
		return (pc & ~(uintptr_t)0xFFF) + (uintptr_t)(sign_extend(imm, 21) << 12) + lo;
	}
	return 0;
}

// Writes count instructions over the image's code (the pages they span), in memory only.
// Shared with steam_live_mapping.c.
int mwxr_write_code(uint32_t *addr, const uint32_t *insns, size_t count)
{
	vm_address_t page = (vm_address_t)addr & ~(vm_address_t)(vm_page_size - 1);
	vm_size_t len = ((vm_address_t)(addr + count) - page + vm_page_size - 1) & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr = vm_protect(mach_task_self(), page, len, FALSE,
	                              VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		mwxr_gate_log("vm_protect(rw) failed: %d", kr);
		return 0;
	}
	memcpy(addr, insns, count * sizeof(*insns));
	kr = vm_protect(mach_task_self(), page, len, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	if (kr != KERN_SUCCESS)
		mwxr_gate_log("vm_protect(rx) failed: %d", kr);
	sys_icache_invalidate(addr, count * sizeof(*insns));
	return 1;
}

static void patch_gate(const struct mach_header_64 *mh, int apply)
{
	uintptr_t strings[MAX_STRINGS];
	int nstrings = find_strings(mh, strings);
	if (nstrings == 0) {
		mwxr_gate_log("no \"linux\" string in steamclient.dylib; Steam Play stays off");
		return;
	}
	unsigned long size = 0;
	uint32_t *w = (uint32_t *)getsectiondata(mh, "__TEXT", "__text", &size);
	size_t n = size / 4;
	uint32_t *site = NULL;
	int sites = 0;
	for (size_t i = 0; w && i < n; i++) {
		uintptr_t target = mwxr_adrp_add_target(w, i, 0);
		if (!target || (w[i] & 0x1F) != 1) // the string goes in x1, the second argument
			continue;
		int is_linux = 0;
		for (int s = 0; s < nstrings; s++)
			is_linux |= target == strings[s];
		if (!is_linux)
			continue;
		for (size_t j = i + 1; j + 3 < n && j <= i + 4; j++) {
			if ((w[j] & 0xFC000000u) != 0x94000000u) // bl
				continue;
			uint32_t cset = w[j + 2], strb = w[j + 3];
			if (w[j + 1] == 0x7100001Fu                        // cmp w0, #0
			    && (cset & 0xFFFFFFE0u) == 0x1A9F17E0u          // cset wN, eq
			    && (strb & 0xFFC00000u) == 0x39000000u          // strb wN, [xM, #imm]
			    && (strb & 0x1F) == (cset & 0x1F)) {
				mwxr_gate_log("candidate at steamclient+0x%lx: field 0x%x",
				         (unsigned long)((uintptr_t)&w[j + 2] - (uintptr_t)mh),
				         (strb >> 10) & 0xFFF);
				site = &w[j + 2];
				sites++;
			}
			break;
		}
	}
	if (sites != 1) {
		mwxr_gate_log("found %d sites, expected 1; Steam Play stays off", sites);
		return;
	}
	uint32_t reg = *site & 0x1F;
	if (!apply)
		return;
	uint32_t mov = 0x52800020u | reg; // movz wN, #1
	if (mwxr_write_code(site, &mov, 1))
		mwxr_gate_log("Steam Play gate open (cset w%u -> mov w%u, #1)", reg, reg);
}

#endif

// steam_live_mapping.c: finds CCompatManager::AddMapping and, with apply, hooks it.
int mwxr_live_mapping_attach(const void *mh, int apply);

static void on_image(const struct mach_header *mh, intptr_t slide)
{
	(void)slide;
	Dl_info info;
	if (!dladdr(mh, &info) || !info.dli_fname)
		return;
	size_t len = strlen(info.dli_fname), tail = strlen(TARGET_IMAGE);
	if (len < tail || strcmp(info.dli_fname + len - tail, TARGET_IMAGE) != 0)
		return;
	mwxr_gate_log("steamclient.dylib loaded: %s", info.dli_fname);
#if defined(__aarch64__)
	patch_gate((const struct mach_header_64 *)mh, 1);
	mwxr_live_mapping_attach(mh, 1);
#else
	mwxr_gate_log("Steam is running as x86_64; only the arm64 client is patched");
#endif
}

__attribute__((constructor)) static void init(void)
{
	if (!in_steam_client())
		return;
	mwxr_gate_log("attached to %s", HOST_PROCESS);
	// Called for images already loaded and for each later one, including the dlopen of
	// steamclient.dylib, which happens before the compatibility manager is built.
	_dyld_register_func_for_add_image(on_image);
}

#if defined(MWXR_GATE_SCAN_MAIN) && defined(__aarch64__)
#include <sys/mman.h>
// Scans an arm64 steamclient.dylib on disk without patching it, logging to
// MWXR_STEAM_COMPAT_GATE_LOG (tests/macos/test_steam_compat_gate.zsh).
int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	int fd = open(argv[1], O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) != 0)
		return 2;
	void *image = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (image == MAP_FAILED)
		return 2;
	patch_gate(image, 0);
	mwxr_live_mapping_attach(image, 0);
	return 0;
}
#endif
