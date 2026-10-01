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
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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
	CCHmac(kCCHmacAlgSHA256, token, MACOS_WINE_XR_TCP_AUTH_TOKEN_SIZE, input, sizeof(input), proof);
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
macos_wine_xr_tcp_auth_token_valid(const char *token)
{
	if (token == NULL || strlen(token) != MACOS_WINE_XR_TCP_AUTH_TOKEN_SIZE) {
		return false;
	}
	for (size_t i = 0; i < MACOS_WINE_XR_TCP_AUTH_TOKEN_SIZE; i++) {
		if (!((token[i] >= '0' && token[i] <= '9') || (token[i] >= 'a' && token[i] <= 'f'))) {
			return false;
		}
	}
	return true;
}

static bool
transfer_exact(int fd, void *bytes, size_t size, bool sending, uint64_t deadline)
{
	size_t offset = 0;
	while (offset < size) {
		uint64_t now = monotonic_ns();
		if (now >= deadline) {
			return false;
		}
		int ms = (int)((deadline - now + 999999ull) / 1000000ull);
		struct pollfd event = {.fd = fd, .events = sending ? POLLOUT : POLLIN};
		int ready = poll(&event, 1, ms);
		if (ready < 0 && errno == EINTR) {
			continue;
		}
		if (ready <= 0) {
			return false;
		}
		ssize_t count = sending
		                  ? send(fd, (char *)bytes + offset, size - offset, MSG_NOSIGNAL)
		                  : recv(fd, (char *)bytes + offset, size - offset, 0);
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
macos_wine_xr_tcp_authenticate_server(int socket_fd, const char *token, int timeout_ms)
{
	if (!macos_wine_xr_tcp_auth_token_valid(token) || timeout_ms <= 0) {
		return false;
	}

	uint64_t deadline = monotonic_ns() + (uint64_t)timeout_ms * 1000000ull;
	unsigned char client[32] = {0};
	unsigned char response[64] = {0};
	unsigned char proof[32] = {0};
	unsigned char expected[32] = {0};

	if (!transfer_exact(socket_fd, client, sizeof(client), false, deadline)) {
		return false;
	}
	arc4random_buf(response, 32);
	if (!make_proof(token, "server", client, response, response + 32) ||
	    !transfer_exact(socket_fd, response, sizeof(response), true, deadline) ||
	    !transfer_exact(socket_fd, proof, sizeof(proof), false, deadline) ||
	    !make_proof(token, "client", client, response, expected) ||
	    !proof_matches(proof, expected)) {
		return false;
	}

	char accepted = 1;
	return transfer_exact(socket_fd, &accepted, 1, true, deadline);
}
