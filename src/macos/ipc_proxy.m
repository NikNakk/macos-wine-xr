// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#import <Metal/Metal.h>

#include "macos_wine_xr/monado_handoff.h"
#include "macos_wine_xr/native_metal_sharing.h"
#include "tcp_auth.h"

#include "proxy_wire_table.h"
#include "shared/ipc_protocol.h"

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

#define PROXY_MAX_REQUEST 8192

static bool
transfer_exact(int fd, void *bytes, size_t size, bool sending)
{
	size_t offset = 0;
	while (offset < size) {
		ssize_t ret = sending ? send(fd, (char *)bytes + offset, size - offset, MSG_NOSIGNAL)
		                      : recv(fd, (char *)bytes + offset, size - offset, 0);
		if (ret < 0 && errno == EINTR) {
			continue;
		}
		if (ret <= 0) {
			return false;
		}
		offset += (size_t)ret;
	}
	return true;
}

static bool
send_framed(int fd, const void *bytes, size_t size)
{
	if (size > UINT32_MAX) {
		return false;
	}
	uint32_t framed_size = (uint32_t)size;
	return transfer_exact(fd, &framed_size, sizeof(framed_size), true) &&
	       transfer_exact(fd, (void *)bytes, size, true);
}

static bool
service_socket_path(char *out, size_t out_size)
{
	const char *explicit_path = getenv("MACOS_WINE_XR_MONADO_SOCKET");
	if (explicit_path != NULL && explicit_path[0] != '\0') {
		return snprintf(out, out_size, "%s", explicit_path) > 0;
	}

	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime != NULL && runtime[0] != '\0') {
		return snprintf(out, out_size, "%s/monado_comp_ipc", runtime) > 0;
	}

	const char *cache = getenv("XDG_CACHE_HOME");
	if (cache != NULL && cache[0] != '\0') {
		return snprintf(out, out_size, "%s/monado/monado_comp_ipc", cache) > 0;
	}

	const char *home = getenv("HOME");
	if (home != NULL && home[0] != '\0') {
		return snprintf(out, out_size, "%s/Library/Caches/monado/monado_comp_ipc", home) > 0;
	}

	return false;
}

static int
connect_service_once(const char *path)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}

	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(addr.sun_path)) {
		close(fd);
		return -1;
	}
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int
connect_service(struct macos_wine_xr_monado_handoff *handoff)
{
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)] = {0};
	if (!service_socket_path(path, sizeof(path))) {
		fprintf(stderr, "could not determine Monado service socket path\n");
		return -1;
	}

	int fd = connect_service_once(path);
	if (fd >= 0) {
		return fd;
	}

	if (macos_wine_xr_monado_activate_service(handoff) != 0) {
		fprintf(stderr, "Monado launchd activation failed\n");
		return -1;
	}

	fd = connect_service_once(path);
	if (fd < 0) {
		fprintf(stderr, "Monado service socket is still unavailable: %s\n", path);
	}
	return fd;
}

static bool
forward_request(int service_fd, const void *request, size_t request_size, void *reply, size_t reply_size)
{
	if (!transfer_exact(service_fd, (void *)request, request_size, true)) {
		return false;
	}
	if (reply_size == 0) {
		return true;
	}
	return transfer_exact(service_fd, reply, reply_size, false);
}

static bool
translate_swapchain_import(int service_fd,
                           int wine_fd,
                           id<MTLDevice> device,
                           struct macos_wine_xr_monado_handoff *handoff,
                           const struct ipc_swapchain_import_metal_bootstrap_msg *old_msg)
{
	uint32_t count = old_msg->args.image_count;
	if (count == 0 || count > XRT_MAX_SWAPCHAIN_IMAGES) {
		return false;
	}

	void *textures[XRT_MAX_SWAPCHAIN_IMAGES] = {0};
	bool ok = true;
	for (uint32_t i = 0; i < count; i++) {
		const char *name = old_msg->args.names[i].name;
		if (memchr(name, '\0', sizeof(old_msg->args.names[i].name)) == NULL ||
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
		macos_wine_xr_release_metal_object(textures[i]);
	}
	if (!ok) {
		return false;
	}

	struct ipc_swapchain_import_metal_msg msg = {
	    .cmd = IPC_SWAPCHAIN_IMPORT_METAL,
	    .info = old_msg->info,
	    .token = token,
	    .image_count = count,
	};
	struct ipc_swapchain_import_metal_reply reply = {0};
	if (!forward_request(service_fd, &msg, sizeof(msg), &reply, sizeof(reply))) {
		return false;
	}
	return send_framed(wine_fd, &reply, sizeof(reply));
}

static bool
translate_semaphore_import(int service_fd,
                           int wine_fd,
                           id<MTLDevice> device,
                           struct macos_wine_xr_monado_handoff *handoff,
                           const struct ipc_compositor_semaphore_import_metal_bootstrap_msg *old_msg)
{
	const char *name = old_msg->bootstrap.name;
	if (memchr(name, '\0', sizeof(old_msg->bootstrap.name)) == NULL) {
		return false;
	}

	void *event = NULL;
	if (macos_wine_xr_resolve_shared_event(name, (__bridge void *)device, &event) != 0) {
		return false;
	}

	uint64_t token = 0;
	int publish_ret = macos_wine_xr_monado_publish_shared_event(handoff, event, &token);
	macos_wine_xr_release_metal_object(event);
	if (publish_ret != 0) {
		return false;
	}

	struct ipc_compositor_semaphore_import_metal_msg msg = {
	    .cmd = IPC_COMPOSITOR_SEMAPHORE_IMPORT_METAL,
	    .token = token,
	};
	struct ipc_compositor_semaphore_import_metal_reply reply = {0};
	if (!forward_request(service_fd, &msg, sizeof(msg), &reply, sizeof(reply))) {
		return false;
	}
	return send_framed(wine_fd, &reply, sizeof(reply));
}

static bool
handle_client(int wine_fd,
              id<MTLDevice> device,
              struct macos_wine_xr_monado_handoff *handoff,
              const char *auth_token)
{
	if (!macos_wine_xr_tcp_authenticate_server(wine_fd, auth_token, 5000)) {
		fprintf(stderr, "rejected unauthenticated Wine connection\n");
		return false;
	}

	int service_fd = connect_service(handoff);
	if (service_fd < 0) {
		return false;
	}

	bool success = true;
	for (;;) {
		enum ipc_command cmd = IPC_ERR;
		ssize_t peeked = recv(wine_fd, &cmd, sizeof(cmd), MSG_PEEK);
		if (peeked == 0) {
			break;
		}
		if (peeked < 0 && errno == EINTR) {
			continue;
		}
		if (peeked != sizeof(cmd)) {
			success = false;
			break;
		}

		size_t request_size = macos_wine_xr_ipc_request_size(cmd);
		size_t reply_size = macos_wine_xr_ipc_reply_size(cmd);
		if (request_size == 0 || request_size > PROXY_MAX_REQUEST) {
			fprintf(stderr, "invalid/unknown IPC command %d\n", (int)cmd);
			success = false;
			break;
		}
		if (macos_wine_xr_ipc_has_native_handles(cmd)) {
			fprintf(stderr, "unsupported native-handle IPC command %s (%d)\n", ipc_cmd_to_str(cmd), (int)cmd);
			success = false;
			break;
		}

		uint8_t request[PROXY_MAX_REQUEST] = {0};
		if (!transfer_exact(wine_fd, request, request_size, false)) {
			success = false;
			break;
		}

		if (cmd == IPC_SWAPCHAIN_IMPORT_METAL_BOOTSTRAP) {
			success = translate_swapchain_import(
			    service_fd, wine_fd, device, handoff,
			    (const struct ipc_swapchain_import_metal_bootstrap_msg *)(const void *)request);
			if (!success) {
				fprintf(stderr, "failed translating Metal texture bootstrap import\n");
				break;
			}
			continue;
		}

		if (cmd == IPC_COMPOSITOR_SEMAPHORE_IMPORT_METAL_BOOTSTRAP) {
			success = translate_semaphore_import(
			    service_fd, wine_fd, device, handoff,
			    (const struct ipc_compositor_semaphore_import_metal_bootstrap_msg *)(const void *)request);
			if (!success) {
				fprintf(stderr, "failed translating Metal shared-event bootstrap import\n");
				break;
			}
			continue;
		}

		if (cmd == IPC_SESSION_CREATE) {
			struct ipc_session_create_msg *msg = (struct ipc_session_create_msg *)(void *)request;
			msg->xsi.pacing_flags |= XRT_SESSION_PACING_USE_MIN_FRAME_PERIOD_BIT;
		}

		uint8_t *reply = NULL;
		if (reply_size > 0) {
			reply = (uint8_t *)calloc(1, reply_size);
			if (reply == NULL) {
				success = false;
				break;
			}
		}

		success = forward_request(service_fd, request, request_size, reply, reply_size);
		if (success && reply_size > 0) {
			success = send_framed(wine_fd, reply, reply_size);
		}
		free(reply);
		if (!success) {
			break;
		}
	}

	close(service_fd);
	return success;
}

int
main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	const char *auth_token = getenv("IPC_WINE_TCP_TOKEN");
	if (!macos_wine_xr_tcp_auth_token_valid(auth_token)) {
		fprintf(stderr, "IPC_WINE_TCP_TOKEN must contain 64 lowercase hexadecimal characters\n");
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

	long port = 4242;
	const char *port_text = getenv("IPC_WINE_TCP_PORT");
	if (port_text != NULL && port_text[0] != '\0') {
		char *end = NULL;
		long parsed = strtol(port_text, &end, 10);
		if (end == port_text || *end != '\0' || parsed <= 0 || parsed > 65535) {
			fprintf(stderr, "invalid IPC_WINE_TCP_PORT: %s\n", port_text);
			[device release];
			macos_wine_xr_monado_handoff_close(handoff);
			return 2;
		}
		port = parsed;
	}

	int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listen_fd < 0) {
		perror("socket");
		[device release];
		macos_wine_xr_monado_handoff_close(handoff);
		return 1;
	}
	int one = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listen_fd, 4) != 0) {
		perror("bind/listen");
		close(listen_fd);
		[device release];
		macos_wine_xr_monado_handoff_close(handoff);
		return 1;
	}

	fprintf(stderr, "macos-wine-xr proxy listening on 127.0.0.1:%ld\n", port);
	for (;;) {
		int client_fd = accept(listen_fd, NULL, NULL);
		if (client_fd < 0) {
			if (errno == EINTR) {
				continue;
			}
			perror("accept");
			break;
		}
		setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		(void)handle_client(client_fd, device, handoff, auth_token);
		close(client_fd);
	}

	close(listen_fd);
	[device release];
	macos_wine_xr_monado_handoff_close(handoff);
	return 0;
}
