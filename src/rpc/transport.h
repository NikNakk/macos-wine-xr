// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "openxr_rpc_generated.h"
#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET mwxr_socket;
#define MWXR_INVALID_SOCKET INVALID_SOCKET
#else
typedef int mwxr_socket;
#define MWXR_INVALID_SOCKET (-1)
#endif
#ifdef __cplusplus
extern "C" {
#endif
struct mwxr_rpc_client {
	mwxr_socket socket;
	uint32_t sequence;
};
void mwxr_rpc_close(mwxr_socket socket);
int mwxr_rpc_connect(struct mwxr_rpc_client *client, const char *port, const char *token);
int mwxr_rpc_call(struct mwxr_rpc_client *client, uint32_t operation, const struct mwxr_wire_request *request,
		  struct mwxr_wire_response *response, int32_t *result);
int mwxr_rpc_receive(mwxr_socket socket, uint32_t *operation, uint32_t *sequence, struct mwxr_wire_request *request);
int mwxr_rpc_respond(mwxr_socket socket, uint32_t operation, uint32_t sequence, int32_t result,
		     const struct mwxr_wire_response *response);

#ifdef __cplusplus
}
#endif
