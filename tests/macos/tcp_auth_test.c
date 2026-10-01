// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "tcp_auth.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *kSecret =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

struct server_args
{
	int fd;
	bool accepted;
};

static void *
server_main(void *opaque)
{
	struct server_args *args = (struct server_args *)opaque;
	args->accepted = mwxr_tcp_authenticate_server(args->fd, kSecret, 1000);
	shutdown(args->fd, SHUT_RDWR);
	return NULL;
}

static int
run_case(const char *client_secret, bool expect_success)
{
	int fd[2] = {-1, -1};
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) != 0) {
		perror("socketpair");
		return 1;
	}

	struct server_args args = {.fd = fd[0], .accepted = false};
	pthread_t thread;
	if (pthread_create(&thread, NULL, server_main, &args) != 0) {
		close(fd[0]);
		close(fd[1]);
		return 1;
	}

	bool client_ok = mwxr_tcp_authenticate_client(fd[1], client_secret, 1000);
	pthread_join(thread, NULL);
	close(fd[0]);
	close(fd[1]);

	if (client_ok != expect_success || args.accepted != expect_success) {
		fprintf(stderr, "auth case failed: expected=%d client=%d server=%d\n",
		        expect_success ? 1 : 0, client_ok ? 1 : 0, args.accepted ? 1 : 0);
		return 1;
	}
	return 0;
}

int
main(void)
{
	if (!mwxr_tcp_auth_token_valid(kSecret) ||
	    mwxr_tcp_auth_token_valid("short")) {
		return 1;
	}
	if (run_case(kSecret, true) != 0) {
		return 1;
	}
	if (run_case("1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", false) != 0) {
		return 1;
	}
	return 0;
}
