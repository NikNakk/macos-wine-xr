// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "transport.h"
#include "tcp_auth.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <ws2tcpip.h>
#define close_socket closesocket
#else
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#define close_socket close
#endif
void mwxr_rpc_close(mwxr_socket socket) {
	if (socket != MWXR_INVALID_SOCKET)
		close_socket(socket);
}
static int transfer(mwxr_socket socket, uint8_t *data, size_t length, int writing) {
	while (length) {
		int n =
		    writing ? send(socket, (char *)data, (int)length, 0) : recv(socket, (char *)data, (int)length, 0);
#ifdef _WIN32
		if (n < 0 && WSAGetLastError() == WSAEINTR)
			continue;
#else
		if (n < 0 && errno == EINTR)
			continue;
#endif
		if (n <= 0)
			return -1;
		data += n;
		length -= (size_t)n;
	}
	return 0;
}
static void put(uint8_t *p, uint64_t value, unsigned n) {
	for (unsigned i = 0; i < n; i++)
		p[i] = (uint8_t)(value >> (8 * i));
}
static uint64_t get(const uint8_t *p, unsigned n) {
	uint64_t v = 0;
	for (unsigned i = 0; i < n; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}
static int packet(mwxr_socket socket, uint32_t *op, uint32_t *seq, int32_t *result, uint8_t *payload, uint32_t size,
		  int writing) {
	uint8_t h[32] = {0};
	if (writing) {
		put(h, MWXR_RPC_MAGIC, 4);
		put(h + 4, MWXR_RPC_VERSION, 4);
		put(h + 8, *op, 4);
		put(h + 12, *seq, 4);
		put(h + 16, (uint32_t)*result, 4);
		put(h + 20, size, 4);
		put(h + 24, MWXR_RPC_FINGERPRINT, 8);
	}
	if (transfer(socket, h, sizeof(h), writing))
		return -1;
	if (get(h, 4) != MWXR_RPC_MAGIC || get(h + 4, 4) != MWXR_RPC_VERSION ||
	    get(h + 24, 8) != MWXR_RPC_FINGERPRINT || get(h + 20, 4) != size ||
	    !mwxr_rpc_operation_valid((uint32_t)get(h + 8, 4)))
		return -1;
	if (!writing) {
		*op = (uint32_t)get(h + 8, 4);
		*seq = (uint32_t)get(h + 12, 4);
		*result = (int32_t)get(h + 16, 4);
	}
	return transfer(socket, payload, size, writing);
}
int mwxr_rpc_connect(struct mwxr_rpc_client *c, const char *port, const char *token) {
	c->socket = MWXR_INVALID_SOCKET;
	c->sequence = 0;
	if (!port || !token || !mwxr_tcp_auth_token_valid(token))
		return -1;
	char *end = NULL;
	long p = strtol(port, &end, 10);
	if (*end || p < 1 || p > 65535)
		return -1;
#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa))
		return -1;
#endif
	mwxr_socket s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == MWXR_INVALID_SOCKET)
		return -1;
	int no_delay = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&no_delay, sizeof(no_delay));
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)p);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifdef _WIN32
	DWORD timeout = 15000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
#else
	struct timeval timeout = {15, 0};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
	if (connect(s, (const struct sockaddr *)&addr, sizeof(addr)) || !mwxr_tcp_authenticate_client(s, token, 5000)) {
		mwxr_rpc_close(s);
		return -1;
	}
	c->socket = s;
	return 0;
}
int mwxr_rpc_receive(mwxr_socket s, uint32_t *op, uint32_t *seq, struct mwxr_wire_request *q) {
	uint8_t data[MWXR_WIRE_REQUEST_SIZE];
	int32_t result = 0;
	if (packet(s, op, seq, &result, data, sizeof(data), 0) || result || !*seq)
		return -1;
	mwxr_decode_request(data, q);
	return 0;
}
int mwxr_rpc_respond(mwxr_socket s, uint32_t op, uint32_t seq, int32_t result, const struct mwxr_wire_response *r) {
	uint8_t data[MWXR_WIRE_RESPONSE_SIZE];
	mwxr_encode_response(data, r);
	return packet(s, &op, &seq, &result, data, sizeof(data), 1);
}
int mwxr_rpc_call(struct mwxr_rpc_client *c, uint32_t op, const struct mwxr_wire_request *q,
		  struct mwxr_wire_response *r, int32_t *result) {
	uint8_t req[MWXR_WIRE_REQUEST_SIZE], rsp[MWXR_WIRE_RESPONSE_SIZE];
	mwxr_encode_request(req, q);
	uint32_t seq = ++c->sequence, reply_op = 0, reply_seq = 0;
	int32_t zero = 0;
	if (!seq || packet(c->socket, &op, &seq, &zero, req, sizeof(req), 1) ||
	    packet(c->socket, &reply_op, &reply_seq, result, rsp, sizeof(rsp), 0) || op != reply_op || seq != reply_seq)
		return -1;
	mwxr_decode_response(rsp, r);
	return 0;
}
