// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "macos_wine_xr/monado_handoff.h"
#include "macos_wine_xr/native_metal_sharing.h"
#include "monado_wire_generated.h"
#include "tcp_auth.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MWXR_DEFAULT_PORT 4242
#define MWXR_MAX_WIRE_MESSAGE 65536

static bool
recv_exact(int fd, void *data, size_t size)
{
	uint8_t *ptr = data;
	size_t offset = 0;
	while (offset < size) {
		ssize_t count = recv(fd, ptr + offset, size - offset, 0);
		if (count < 0 && errno == EINTR) {
			continue;
		}
		if (count <= 0) {
			return false;
		}
		offset += (size_t)count;
	}
	return true;
}

static bool
send_exact(int fd, const void *data, size_t size)
{
	const uint8_t *ptr = data;
	size_t offset = 0;
	while (offset < size) {
#ifdef MSG_NOSIGNAL
		ssize_t count = send(fd, ptr + offset, size - offset, MSG_NOSIGNAL);
#else
		ssize_t count = send(fd, ptr + offset, size - offset, 0);
#endif
		if (count < 0 && errno == EINTR) {
			continue;
		}
		if (count <= 0) {
			return false;
		}
		offset += (size_t)count;
	}
	return true;
}

static bool
send_framed_reply(int fd, const void *reply, size_t size)
{
	if (size > UINT32_MAX) {
		return false;
	}
	uint32_t wire_size = (uint32_t)size;
	return send_exact(fd, &wire_size, sizeof(wire_size)) && send_exact(fd, reply, size);
}

static bool
runtime_socket_path(char *out, size_t out_size)
{
	const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
	if (xdg_runtime != NULL && xdg_runtime[0] != '\0') {
		return snprintf(out, out_size, "%s/monado_comp_ipc", xdg_runtime) > 0 &&
		       strlen(out) < out_size;
	}

	const char *xdg_cache = getenv("XDG_CACHE_HOME");
	if (xdg_cache != NULL && xdg_cache[0] != '\0') {
		return snprintf(out, out_size, "%s/monado/monado_comp_ipc", xdg_cache) > 0 &&
		       strlen(out) < out_size;
	}

	const char *home = getenv("HOME");
	if (home == NULL || home[0] == '\0') {
		return false;
	}
	return snprintf(out, out_size, "%s/Library/Caches/monado/monado_comp_ipc", home) > 0 &&
	       strlen(out) < out_size;
}

static int
connect_native_socket(const char *path)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}

	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(addr.sun_path)) {
		close(fd);
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(addr.sun_path, path, strlen(path) + 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int
connect_native_with_activation(struct macos_wine_xr_monado_handoff *handoff, const char *socket_path)
{
	int fd = connect_native_socket(socket_path);
	if (fd >= 0) {
		return fd;
	}

	if (macos_wine_xr_monado_activate_service(handoff) != 0) {
		return -1;
	}
	return connect_native_socket(socket_path);
}

static int
create_listener(uint16_t port)
{
	int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) {
		return -1;
	}

	int one = 1;
	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool
translate_texture_import(uint8_t *request,
                         size_t request_size,
                         id<MTLDevice> device,
                         struct macos_wine_xr_monado_handoff *handoff,
                         uint8_t *out_request,
                         size_t out_capacity,
                         size_t *out_size)
{
	if (request_size != sizeof(struct ipc_swapchain_import_metal_bootstrap_msg) ||
	    out_capacity < sizeof(struct ipc_swapchain_import_metal_msg)) {
		return false;
	}

	const struct ipc_swapchain_import_metal_bootstrap_msg *old_msg =
	    (const struct ipc_swapchain_import_metal_bootstrap_msg *)request;
	uint32_t count = old_msg->args.image_count;
	if (count == 0 || count > XRT_MAX_SWAPCHAIN_IMAGES) {
		return false;
	}

	void *textures[XRT_MAX_SWAPCHAIN_IMAGES] = {0};
	bool ok = true;
	for (uint32_t i = 0; i < count; i++) {
		const char *name = old_msg->args.names[i].name;
		if (name[0] == '\0' || memchr(name, '\0', sizeof(old_msg->args.names[i].name)) == NULL ||
		    macos_wine_xr_resolve_shared_texture(name, (__bridge void *)device, &textures[i]) != 0) {
			ok = false;
			break;
		}
	}

	uint64_t token = 0;
	if (ok) {
		ok = macos_wine_xr_monado_publish_textures(handoff, textures, count, &token) == 0;
	}

	for (uint32_t i = 0; i < count; i++) {
		if (textures[i] != NULL) {
			macos_wine_xr_release_metal_object(textures[i]);
		}
	}
	if (!ok) {
		return false;
	}

	struct ipc_swapchain_import_metal_msg translated = {
	    .cmd = IPC_SWAPCHAIN_IMPORT_METAL,
	    .info = old_msg->info,
	    .token = token,
	    .image_count = count,
	};
	memcpy(out_request, &translated, sizeof(translated));
	*out_size = sizeof(translated);
	return true;
}

static bool
translate_semaphore_import(uint8_t *request,
                           size_t request_size,
                           id<MTLDevice> device,
                           struct macos_wine_xr_monado_handoff *handoff,
                           uint8_t *out_request,
                           size_t out_capacity,
                           size_t *out_size)
{
	if (request_size != sizeof(struct ipc_compositor_semaphore_import_metal_bootstrap_msg) ||
	    out_capacity < sizeof(struct ipc_compositor_semaphore_import_metal_msg)) {
		return false;
	}

	const struct ipc_compositor_semaphore_import_metal_bootstrap_msg *old_msg =
	    (const struct ipc_compositor_semaphore_import_metal_bootstrap_msg *)request;
	const char *name = old_msg->bootstrap.name;
	if (name[0] == '\0' || memchr(name, '\0', sizeof(old_msg->bootstrap.name)) == NULL) {
		return false;
	}

	void *event = NULL;
	if (macos_wine_xr_resolve_shared_event(name, (__bridge void *)device, &event) != 0) {
		return false;
	}

	uint64_t token = 0;
	bool ok = macos_wine_xr_monado_publish_shared_event(handoff, event, &token) == 0;
	macos_wine_xr_release_metal_object(event);
	if (!ok) {
		return false;
	}

	struct ipc_compositor_semaphore_import_metal_msg translated = {
	    .cmd = IPC_COMPOSITOR_SEMAPHORE_IMPORT_METAL,
	    .token = token,
	};
	memcpy(out_request, &translated, sizeof(translated));
	*out_size = sizeof(translated);
	return true;
}

static bool
proxy_one_client(int wine_fd,
                 int native_fd,
                 id<MTLDevice> device,
                 struct macos_wine_xr_monado_handoff *handoff)
{
	uint8_t request[MWXR_MAX_WIRE_MESSAGE] = {0};
	uint8_t translated[MWXR_MAX_WIRE_MESSAGE] = {0};
	uint8_t reply[MWXR_MAX_WIRE_MESSAGE] = {0};

	for (;;) {
		uint32_t cmd = 0;
		ssize_t first = recv(wine_fd, &cmd, sizeof(cmd), MSG_WAITALL);
		if (first == 0) {
			return true;
		}
		if (first != sizeof(cmd)) {
			return false;
		}

		struct mwxr_wire_info info = mwxr_wire_info_for(cmd);
		if (info.request_size < sizeof(cmd) || info.request_size > sizeof(request) ||
		    info.reply_size > sizeof(reply)) {
			fprintf(stderr, "proxy: unknown/oversized command %u\n", cmd);
			return false;
		}
		if (info.in_handles || info.out_handles) {
			fprintf(stderr, "proxy: rejecting native-handle command %u\n", cmd);
			return false;
		}

		memcpy(request, &cmd, sizeof(cmd));
		if (info.request_size > sizeof(cmd) &&
		    !recv_exact(wine_fd, request + sizeof(cmd), info.request_size - sizeof(cmd))) {
			return false;
		}

		const uint8_t *native_request = request;
		size_t native_request_size = info.request_size;
		if (cmd == IPC_SWAPCHAIN_IMPORT_METAL_BOOTSTRAP) {
			if (!translate_texture_import(request, info.request_size, device, handoff,
			                              translated, sizeof(translated), &native_request_size)) {
				fprintf(stderr, "proxy: texture bootstrap translation failed\n");
				return false;
			}
			native_request = translated;
		} else if (cmd == IPC_COMPOSITOR_SEMAPHORE_IMPORT_METAL_BOOTSTRAP) {
			if (!translate_semaphore_import(request, info.request_size, device, handoff,
			                                translated, sizeof(translated), &native_request_size)) {
				fprintf(stderr, "proxy: semaphore bootstrap translation failed\n");
				return false;
			}
			native_request = translated;
		} else if (cmd == IPC_SESSION_CREATE) {
			/*
			 * This was previously server policy for stream_socket clients.
			 * The compatibility proxy owns that policy now: the native Monado
			 * service sees an ordinary Unix-socket client.
			 */
			struct ipc_session_create_msg *msg = (struct ipc_session_create_msg *)request;
			msg->xsi.pacing_flags |= XRT_SESSION_PACING_USE_MIN_FRAME_PERIOD_BIT;
		}

		if (!send_exact(native_fd, native_request, native_request_size)) {
			return false;
		}

		if (info.one_way) {
			continue;
		}
		if (info.reply_size == 0 || !recv_exact(native_fd, reply, info.reply_size) ||
		    !send_framed_reply(wine_fd, reply, info.reply_size)) {
			return false;
		}
	}
}

static bool
parse_port(const char *text, uint16_t *out_port)
{
	if (text == NULL || text[0] == '\0' || out_port == NULL) {
		return false;
	}
	char *end = NULL;
	unsigned long value = strtoul(text, &end, 10);
	if (end == text || *end != '\0' || value == 0 || value > 65535) {
		return false;
	}
	*out_port = (uint16_t)value;
	return true;
}

int
main(int argc, char **argv)
{
	const char *token = getenv("IPC_WINE_TCP_TOKEN");
	if (!mwxr_tcp_auth_token_valid(token)) {
		fprintf(stderr, "IPC_WINE_TCP_TOKEN must be 64 lowercase hexadecimal characters\n");
		return 2;
	}

	uint16_t port = MWXR_DEFAULT_PORT;
	const char *port_text = getenv("IPC_WINE_TCP_PORT");
	if (port_text != NULL && !parse_port(port_text, &port)) {
		fprintf(stderr, "invalid IPC_WINE_TCP_PORT: %s\n", port_text);
		return 2;
	}

	const char *socket_path_env = getenv("MONADO_IPC_SOCKET");
	char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)] = {0};
	if (socket_path_env != NULL && socket_path_env[0] != '\0') {
		if (strlen(socket_path_env) >= sizeof(socket_path)) {
			fprintf(stderr, "MONADO_IPC_SOCKET is too long\n");
			return 2;
		}
		strcpy(socket_path, socket_path_env);
	} else if (!runtime_socket_path(socket_path, sizeof(socket_path))) {
		fprintf(stderr, "could not determine Monado runtime socket path\n");
		return 2;
	}

	const char *dylib_path = getenv("MONADO_METAL_XPC_CLIENT");
	struct macos_wine_xr_monado_handoff *handoff = NULL;
	if (macos_wine_xr_monado_handoff_open(dylib_path, &handoff) != 0) {
		fprintf(stderr, "could not load libmonado_metal_xpc_client.dylib\n");
		return 2;
	}

	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (device == nil) {
		fprintf(stderr, "no default Metal device\n");
		macos_wine_xr_monado_handoff_close(handoff);
		return 2;
	}

	int listener = create_listener(port);
	if (listener < 0) {
		fprintf(stderr, "could not listen on 127.0.0.1:%u: %s\n", port, strerror(errno));
		[device release];
		macos_wine_xr_monado_handoff_close(handoff);
		return 2;
	}

	fprintf(stderr, "macos-wine-xr proxy listening on 127.0.0.1:%u -> %s\n", port, socket_path);

	for (;;) {
		int wine_fd = accept(listener, NULL, NULL);
		if (wine_fd < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}

		int one = 1;
		(void)setsockopt(wine_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

		if (!mwxr_tcp_authenticate_server(wine_fd, token, 1000)) {
			fprintf(stderr, "proxy: rejected unauthenticated Wine client\n");
			close(wine_fd);
			continue;
		}

		int native_fd = connect_native_with_activation(handoff, socket_path);
		if (native_fd < 0) {
			fprintf(stderr, "proxy: could not connect to native Monado service: %s\n", strerror(errno));
			close(wine_fd);
			continue;
		}

		bool clean = proxy_one_client(wine_fd, native_fd, device, handoff);
		close(native_fd);
		close(wine_fd);
		fprintf(stderr, "proxy: Wine client disconnected%s\n", clean ? "" : " after IPC failure");
	}

	close(listener);
	[device release];
	macos_wine_xr_monado_handoff_close(handoff);
	return 1;
}
