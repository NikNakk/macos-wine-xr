// Worker-style waits: WaitForMultipleObjects on {semaphore, never-signalled
// event} while other threads release the semaphore, to reach msync's
// contention and already-signalled paths.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
static HANDLE sem, quit;
static volatile LONG stop;
static DWORD WINAPI producer(void *p) { while (!stop) ReleaseSemaphore(sem, 1, NULL); return 0; }
static DWORD WINAPI consumer(void *p) {
	HANDLE h[2] = {quit, sem}; long n = 0;
	while (!stop) { if (WaitForMultipleObjects(2, h, FALSE, 50) == WAIT_OBJECT_0 + 1) n++; }
	return (DWORD)n;
}
int main(int argc, char **argv) {
	int seconds = argc > 1 ? atoi(argv[1]) : 20;
	sem = CreateSemaphoreA(NULL, 0, 0x7fffffff, NULL); quit = CreateEventA(NULL, TRUE, FALSE, NULL);
	HANDLE t[12];
	for (int i = 0; i < 4; i++) t[i] = CreateThread(NULL, 0, producer, NULL, 0, NULL);
	for (int i = 4; i < 12; i++) t[i] = CreateThread(NULL, 0, consumer, NULL, 0, NULL);
	Sleep(seconds * 1000); stop = 1; SetEvent(quit);
	long total = 0; DWORD code;
	for (int i = 4; i < 12; i++) { WaitForSingleObject(t[i], INFINITE); GetExitCodeThread(t[i], &code); total += code; }
	printf("waits satisfied: %ld in %d s\n", total, seconds);
	return 0;
}
