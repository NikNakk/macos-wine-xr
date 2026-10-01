// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "tcp_auth.h"

#include <CommonCrypto/CommonHMAC.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

static uint64_t
monotonic_ns(void)
{
	struct timespec ts = {0};
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static bool
random_nonce(unsigned char nonce[32])
{
	arc4random_buf(nonce, 32);
	return true;
}

static bool
make_proof(const char *token,
           const char role[6],
           const unsigned char client[32],
           const unsigned char server[32],
           unsigned char proof[32])
{
	unsigned char input[70];
	memcpy(input, role, 6);
	memcpy(input + 6, client, 32);
	memcpy(input + 38, server, 32);
	CCHmac(kCCHmacAlgSHA256, token, MWXR_TCP_AUTH_TOKEN_SIZE, input, sizeof(input), proof);
	return true;
}

static bool
proof_matches(const unsigned char a[32], const unsigned char b[32])
{
	unsigned difference = 0;
	for (size_t i = 0; i < 32; i++) {
		difference |= a[i] ^ b[i];
	}
	return difference == 0;
}

bool
mwxr_tcp_auth_token_valid(const char *token)
{
	if (token == NULL || strlen(token) != MWXR_TCP_AUTH_TOKEN_SIZE) {
		return false;
	}
	for (size_t i = 0; i < MWXR_TCP_AUTH_TOKEN_SIZE; i++) {
		if (!((token[i] >= '0' && token[i] <= '9') || (token[i] >= 'a' && token[i] <= 'f'))) {
			return false;
		}
	}
	return true;
}

static bool
transfer(int socket_fd, char *bytes, size_t size, bool sending, uint64_t deadline)
{
	size_t offset = 0;
	while (offset < size) {
		uint64_t now = monotonic_ns();
		if (now == 0 || now >= deadline) {
			return false;
		}
		int ms = (int)((deadline - now + 999999ull) / 1000000ull);
		struct pollfd event = {
		    .fd = socket_fd,
		    .events = sending ? POLLOUT : POLLIN,
		};
		int ready = poll(&event, 1, ms);
		if (ready < 0 && errno == EINTR) {
			continue;
		}
		if (ready <= 0) {
			return false;
		}

		int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
		if (sending) {
			flags |= MSG_NOSIGNAL;
		}
#endif
		ssize_t count = sending ? send(socket_fd, bytes + offset, size - offset, flags)
		                        : recv(socket_fd, bytes + offset, size - offset, flags);
		if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
			continue;
		}
		if (count <= 0) {
			return false;
		}
		offset += (size_t)count;
	}
	return true;
}

bool
mwxr_tcp_authenticate_server(int socket_fd, const char *token, int timeout_ms)
{
	if (!mwxr_tcp_auth_token_valid(token) || timeout_ms <= 0) {
		return false;
	}
	uint64_t now = monotonic_ns();
	if (now == 0) {
		return false;
	}
	uint64_t deadline = now + (uint64_t)timeout_ms * 1000000ull;
	unsigned char client[32] = {0};
	unsigned char response[64] = {0};
	unsigned char proof[32] = {0};
	unsigned char expected[32] = {0};

	if (!transfer(socket_fd, (char *)client, sizeof(client), false, deadline) ||
	    !random_nonce(response) ||
	    !make_proof(token, "server", client, response, response + 32) ||
	    !transfer(socket_fd, (char *)response, sizeof(response), true, deadline) ||
	    !transfer(socket_fd, (char *)proof, sizeof(proof), false, deadline) ||
	    !make_proof(token, "client", client, response, expected) ||
	    !proof_matches(proof, expected)) {
		return false;
	}

	char accepted = 1;
	return transfer(socket_fd, &accepted, 1, true, deadline);
}
