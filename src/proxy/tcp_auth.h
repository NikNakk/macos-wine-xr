// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <stdbool.h>

#define MWXR_TCP_AUTH_TOKEN_SIZE 64

bool
mwxr_tcp_auth_token_valid(const char *token);

bool
mwxr_tcp_authenticate_server(int socket_fd, const char *token, int timeout_ms);
