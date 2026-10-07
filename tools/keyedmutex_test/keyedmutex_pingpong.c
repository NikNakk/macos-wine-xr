// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// Two processes pass a shared D3DKMT keyed mutex back and forth, the way
// SteamVR's client and compositor hand over a submitted texture. Checks a
// Wine build's keyed mutexes, for example with and without msync.
//
//   keyedmutex_pingpong.exe [rounds]
//
// The parent creates the mutex (key 0), starts a child with its shared
// handle, and loops: acquire key 0, release key 1. The child loops: acquire
// key 1, release key 0. Every acquire has a 1 s timeout. Both report round
// trips, timeouts and failures.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef UINT D3DKMT_HANDLE;
typedef LONG NTSTATUS_T;

typedef struct
{
	UINT64 InitialValue;
	D3DKMT_HANDLE hSharedHandle;
	D3DKMT_HANDLE hKeyedMutex;
} CREATEKEYEDMUTEX;

typedef struct
{
	D3DKMT_HANDLE hSharedHandle;
	D3DKMT_HANDLE hKeyedMutex;
} OPENKEYEDMUTEX;

typedef struct
{
	D3DKMT_HANDLE hKeyedMutex;
	UINT64 Key;
	LARGE_INTEGER *pTimeout;
	UINT64 FenceValue;
} ACQUIREKEYEDMUTEX;

typedef struct
{
	D3DKMT_HANDLE hKeyedMutex;
	UINT64 Key;
	UINT64 FenceValue;
} RELEASEKEYEDMUTEX;

typedef NTSTATUS_T(WINAPI *CreateFn)(CREATEKEYEDMUTEX *);
typedef NTSTATUS_T(WINAPI *OpenFn)(OPENKEYEDMUTEX *);
typedef NTSTATUS_T(WINAPI *AcquireFn)(ACQUIREKEYEDMUTEX *);
typedef NTSTATUS_T(WINAPI *ReleaseFn)(RELEASEKEYEDMUTEX *);

static AcquireFn acquire_fn;
static ReleaseFn release_fn;

static double
now_ms(void)
{
	static LARGE_INTEGER frequency;
	LARGE_INTEGER t;
	if (!frequency.QuadPart) {
		QueryPerformanceFrequency(&frequency);
	}
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

// Acquire @p take and release @p give, @p rounds times.
static int
loop(const char *who, D3DKMT_HANDLE mutex, UINT64 take, UINT64 give, int rounds)
{
	int timeouts = 0, failures = 0;
	double worst = 0, start = now_ms();
	for (int i = 0; i < rounds; i++) {
		LARGE_INTEGER timeout = {.QuadPart = -10000000LL}; // 1 s, relative
		ACQUIREKEYEDMUTEX a = {mutex, take, &timeout, 0};
		double before = now_ms();
		NTSTATUS_T status = acquire_fn(&a);
		double took = now_ms() - before;
		worst = took > worst ? took : worst;
		if (status == 0x102 /* STATUS_TIMEOUT */) {
			++timeouts;
			if (timeouts <= 3) {
				printf("%s: round %d: acquire key %llu timed out\n", who, i, (unsigned long long)take);
			}
			continue;
		}
		if (status != 0) {
			if (++failures <= 3) {
				printf("%s: round %d: acquire failed 0x%lx\n", who, i, (unsigned long)status);
			}
			continue;
		}
		RELEASEKEYEDMUTEX r = {mutex, give, 0};
		status = release_fn(&r);
		if (status != 0 && ++failures <= 3) {
			printf("%s: round %d: release failed 0x%lx\n", who, i, (unsigned long)status);
		}
	}
	double total = now_ms() - start;
	printf("%s: %d rounds in %.1f ms (%.3f ms each), slowest acquire %.2f ms, timeouts %d, failures %d\n", who,
	       rounds, total, total / rounds, worst, timeouts, failures);
	fflush(stdout);
	return timeouts || failures;
}

int
main(int argc, char **argv)
{
	HMODULE gdi = LoadLibraryA("gdi32.dll");
	CreateFn create_fn = (CreateFn)GetProcAddress(gdi, "D3DKMTCreateKeyedMutex");
	OpenFn open_fn = (OpenFn)GetProcAddress(gdi, "D3DKMTOpenKeyedMutex");
	acquire_fn = (AcquireFn)GetProcAddress(gdi, "D3DKMTAcquireKeyedMutex");
	release_fn = (ReleaseFn)GetProcAddress(gdi, "D3DKMTReleaseKeyedMutex");
	if (!create_fn || !open_fn || !acquire_fn || !release_fn) {
		printf("D3DKMT keyed mutex functions missing\n");
		return 1;
	}

	if (argc >= 3 && !strcmp(argv[1], "--child")) {
		OPENKEYEDMUTEX o = {(D3DKMT_HANDLE)strtoul(argv[2], NULL, 0), 0};
		NTSTATUS_T status = open_fn(&o);
		if (status) {
			printf("child: open failed 0x%lx\n", (unsigned long)status);
			return 1;
		}
		return loop("child", o.hKeyedMutex, 1, 0, argc >= 4 ? atoi(argv[3]) : 1000);
	}

	int rounds = argc >= 2 ? atoi(argv[1]) : 1000;
	CREATEKEYEDMUTEX c = {0, 0, 0};
	NTSTATUS_T status = create_fn(&c);
	if (status) {
		printf("parent: create failed 0x%lx\n", (unsigned long)status);
		return 1;
	}
	char exe[MAX_PATH], cmd[MAX_PATH + 64];
	GetModuleFileNameA(NULL, exe, sizeof(exe));
	snprintf(cmd, sizeof(cmd), "\"%s\" --child 0x%x %d", exe, c.hSharedHandle, rounds);
	STARTUPINFOA si = {sizeof(si)};
	PROCESS_INFORMATION pi;
	if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		printf("parent: CreateProcess failed %lu\n", GetLastError());
		return 1;
	}
	int bad = loop("parent", c.hKeyedMutex, 0, 1, rounds);
	WaitForSingleObject(pi.hProcess, 30000);
	DWORD child = 1;
	GetExitCodeProcess(pi.hProcess, &child);
	return bad || child;
}
