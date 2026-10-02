// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <stdbool.h>
#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET mwxr_auth_socket;
#else
typedef int mwxr_auth_socket;
#endif

#define MWXR_TCP_AUTH_TOKEN_SIZE 64

bool
mwxr_tcp_auth_token_valid(const char *token);

bool
mwxr_tcp_authenticate_server(mwxr_auth_socket socket_fd, const char *token, int timeout_ms);

bool
mwxr_tcp_authenticate_client(mwxr_auth_socket socket_fd, const char *token, int timeout_ms);
