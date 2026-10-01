// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <stdbool.h>

#define MACOS_WINE_XR_TCP_AUTH_TOKEN_SIZE 64

bool
macos_wine_xr_tcp_auth_token_valid(const char *token);

bool
macos_wine_xr_tcp_authenticate_server(int socket_fd, const char *token, int timeout_ms);
