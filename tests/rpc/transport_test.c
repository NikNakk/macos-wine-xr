// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "transport.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
static void put(uint8_t *b, uint64_t v, unsigned n) {
	for (unsigned i = 0; i < n; i++)
		b[i] = (uint8_t)(v >> (8 * i));
}
static void *respond(void *arg) {
	int fd = *(int *)arg;
	uint32_t op, seq;
	struct mwxr_wire_request q;
	struct mwxr_wire_response r = {0};
	if (mwxr_rpc_receive(fd, &op, &seq, &q) || op != MWXR_OP_PROPERTIES || seq != 1 || q.object != 7)
		return (void *)1;
	r.object = 9;
	return (void *)(intptr_t)mwxr_rpc_respond(fd, op, seq, -12, &r);
}
int main(void) {
	int sockets[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets))
		return 1;
	pthread_t thread;
	pthread_create(&thread, NULL, respond, &sockets[1]);
	struct mwxr_rpc_client client = {sockets[0], 0};
	struct mwxr_wire_request q = {.object = 7};
	struct mwxr_wire_response r;
	int32_t result;
	int failed = mwxr_rpc_call(&client, MWXR_OP_PROPERTIES, &q, &r, &result) || result != -12 || r.object != 9;
	void *returned;
	pthread_join(thread, &returned);
	failed |= returned != NULL;
	close(sockets[0]);
	close(sockets[1]);
	// Wrong version, oversized payload, wrong fingerprint and invalid operation
	// must fail from the header without waiting for a declared payload.
	for (unsigned test = 0; test < 4; test++) {
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets))
			return 1;
		uint8_t h[32] = {0};
		put(h, MWXR_RPC_MAGIC, 4);
		put(h + 4, MWXR_RPC_VERSION, 4);
		put(h + 8, MWXR_OP_HELLO, 4);
		put(h + 12, 1, 4);
		put(h + 20, MWXR_WIRE_REQUEST_SIZE, 4);
		put(h + 24, MWXR_RPC_FINGERPRINT, 8);
		if (test == 0)
			put(h + 4, 99, 4);
		if (test == 1)
			put(h + 20, 0xffffffff, 4);
		if (test == 2)
			put(h + 24, 0, 8);
		if (test == 3)
			put(h + 8, 0, 4);
		if (write(sockets[0], h, 32) != 32)
			return 1;
		uint32_t op, seq;
		failed |= mwxr_rpc_receive(sockets[1], &op, &seq, &q) == 0;
		close(sockets[0]);
		close(sockets[1]);
	}
	puts(failed ? "RPC framing failed" : "RPC framing and mismatch rejection passed");
	return failed;
}
