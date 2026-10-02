// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "tcp_auth.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#else
#include <CommonCrypto/CommonHMAC.h>
#include <poll.h>
#include <sys/socket.h>
#endif
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t
monotonic_ns(void)
{
	#ifdef _WIN32
	return GetTickCount64() * 1000000ull;
#else
	struct timespec ts = {0};
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static bool
random_nonce(unsigned char nonce[32])
{
	#ifdef _WIN32
	return BCryptGenRandom(NULL, nonce, 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
	arc4random_buf(nonce, 32);
	return true;
#endif
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
	#ifdef _WIN32
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
	if (!status) status = BCryptCreateHash(alg, &hash, NULL, 0, (PUCHAR)token, MWXR_TCP_AUTH_TOKEN_SIZE, 0);
	if (!status) status = BCryptHashData(hash, input, sizeof(input), 0);
	if (!status) status = BCryptFinishHash(hash, proof, 32, 0);
	if (hash) BCryptDestroyHash(hash);
	if (alg) BCryptCloseAlgorithmProvider(alg, 0);
	return status == 0;
#else
	CCHmac(kCCHmacAlgSHA256, token, MWXR_TCP_AUTH_TOKEN_SIZE, input, sizeof(input), proof);
	return true;
#endif
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
transfer(mwxr_auth_socket socket_fd, char *bytes, size_t size, bool sending, uint64_t deadline)
{
	size_t offset = 0;
	while (offset < size) {
		uint64_t now = monotonic_ns();
		if (now == 0 || now >= deadline) {
			return false;
		}
		int ms = (int)((deadline - now + 999999ull) / 1000000ull);
		#ifdef _WIN32
		WSAPOLLFD event = {
#else
		struct pollfd event = {
#endif
		    .fd = socket_fd,
		    .events = sending ? POLLOUT : POLLIN,
		};
		#ifdef _WIN32
		int ready = WSAPoll(&event, 1, ms);
#else
		int ready = poll(&event, 1, ms);
#endif
		if (ready < 0 && errno == EINTR) {
			continue;
		}
		if (ready <= 0) {
			return false;
		}

		#ifdef _WIN32
		int flags = 0;
#else
		int flags = MSG_DONTWAIT;
#endif
#ifdef MSG_NOSIGNAL
		if (sending) {
			flags |= MSG_NOSIGNAL;
		}
#endif
		int count = sending ? send(socket_fd, bytes + offset, (int)(size - offset), flags)
		                        : recv(socket_fd, bytes + offset, (int)(size - offset), flags);
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
mwxr_tcp_authenticate_server(mwxr_auth_socket socket_fd, const char *token, int timeout_ms)
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

bool
mwxr_tcp_authenticate_client(mwxr_auth_socket socket_fd, const char *token, int timeout_ms)
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
	unsigned char expected[32] = {0};
	unsigned char proof[32] = {0};

	if (!random_nonce(client) ||
	    !transfer(socket_fd, (char *)client, sizeof(client), true, deadline) ||
	    !transfer(socket_fd, (char *)response, sizeof(response), false, deadline) ||
	    !make_proof(token, "server", client, response, expected) ||
	    !proof_matches(response + 32, expected) ||
	    !make_proof(token, "client", client, response, proof) ||
	    !transfer(socket_fd, (char *)proof, sizeof(proof), true, deadline)) {
		return false;
	}

	char accepted = 0;
	return transfer(socket_fd, &accepted, 1, false, deadline) && accepted == 1;
}
