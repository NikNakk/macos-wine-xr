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
#include <sys/mman.h>
#include <sys/stat.h>
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
		return snprintf(out, out_size, "%s/monado_comp_ipc", xdg_runtime) > 0 && strlen(out) < out_size;
	}

	const char *xdg_cache = getenv("XDG_CACHE_HOME");
	if (xdg_cache != NULL && xdg_cache[0] != '\0') {
		return snprintf(out, out_size, "%s/monado/monado_comp_ipc", xdg_cache) > 0 && strlen(out) < out_size;
	}

	const char *home = getenv("HOME");
	if (home == NULL || home[0] == '\0') {
		return false;
	}
	return snprintf(out, out_size, "%s/Library/Caches/monado/monado_comp_ipc", home) > 0 && strlen(out) < out_size;
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

	fprintf(stderr, "proxy: texture import images=%u path=shared-metal-zero-copy pixel-copies=0 gpu-blits=0\n",
	        count);
	struct ipc_swapchain_import_metal_msg translated = {
	    .cmd = NATIVE_IPC_SWAPCHAIN_IMPORT_METAL,
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

	fprintf(stderr, "proxy: shared-event import path=native-token\n");
	struct ipc_compositor_semaphore_import_metal_msg translated = {
	    .cmd = NATIVE_IPC_COMPOSITOR_SEMAPHORE_IMPORT_METAL,
	    .token = token,
	};
	memcpy(out_request, &translated, sizeof(translated));
	*out_size = sizeof(translated);
	return true;
}

/* Only metadata crosses this connection. Metal resources use the XPC token path. */
static struct ipc_shared_memory *
map_native_shm(int fd)
{
	uint32_t cmd = NATIVE_IPC_INSTANCE_GET_SHM_FD;
	if (!send_exact(fd, &cmd, sizeof(cmd))) {
		return NULL;
	}
	struct mwxr_ipc_result_reply reply = {0};
	union {
		struct cmsghdr align;
		char bytes[CMSG_SPACE(sizeof(int) * 8)];
	} control;
	int shm_fd = -1;
	size_t received = 0;
	bool valid = true;
	while (received < sizeof(reply)) {
		struct iovec iov = {(uint8_t *)&reply + received, sizeof(reply) - received};
		struct msghdr msg = {0};
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = control.bytes;
		msg.msg_controllen = sizeof(control.bytes);
		ssize_t n = recvmsg(fd, &msg, 0);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			valid = false;
			break;
		}
		if (msg.msg_flags & MSG_CTRUNC) {
			valid = false;
		}
		for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
			if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
				continue;
			}
			size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			int *fds = (int *)CMSG_DATA(c);
			for (size_t i = 0; i < count; i++) {
				if (shm_fd < 0) {
					shm_fd = fds[i];
				} else {
					close(fds[i]);
					valid = false;
				}
			}
		}
		received += (size_t)n;
	}
	struct stat st;
	struct ipc_shared_memory *ism = MAP_FAILED;
	if (valid && reply.result == XRT_SUCCESS && shm_fd >= 0 && fstat(shm_fd, &st) == 0 &&
	    st.st_size >= (off_t)sizeof(*ism)) {
		ism = mmap(NULL, sizeof(*ism), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	}
	if (shm_fd >= 0) {
		close(shm_fd);
	}
	return ism == MAP_FAILED ? NULL : ism;
}

#define MWXR_MAX_METADATA_BYTES (64u * 1024u * 1024u)

/* Native variable-length calls have separate logical blocks, not TCP frames.
 * Reconstruct each block from its protocol count before framing it for Wine. */
static bool
forward_metadata_block(int native_fd, int wine_fd, uint64_t count, uint64_t stride)
{
	if (count == 0) {
		return true;
	}
	if (stride == 0 || count > MWXR_MAX_METADATA_BYTES / stride) {
		return false;
	}
	size_t size = (size_t)(count * stride);
	if (size == 0) {
		return true;
	}
	void *data = malloc(size);
	if (data == NULL) {
		return false;
	}
	bool ok = recv_exact(native_fd, data, size) && send_framed_reply(wine_fd, data, size);
	free(data);
	return ok;
}

struct proxy_device_info
{
	uint32_t id;
	struct ipc_device_info info;
	bool valid;
};

static bool
forward_varlen(int native_fd, int wine_fd, uint32_t cmd, const void *request, struct proxy_device_info *devices)
{
	if (cmd == IPC_DEVICE_GET_INFO || cmd == IPC_DEVICE_GET_INFO_NO_ARRAYS) {
		const struct ipc_device_get_info_msg *msg = request;
		struct ipc_device_get_info_reply reply;
		if (!recv_exact(native_fd, &reply, sizeof(reply)) ||
		    !send_framed_reply(wine_fd, &reply, sizeof(reply))) {
			return false;
		}
		if (reply.result != XRT_SUCCESS) {
			return true;
		}
		struct ipc_device_info *info = &reply.info;
		struct proxy_device_info *entry = NULL;
		for (uint32_t i = 0; i < XRT_SYSTEM_MAX_DEVICES; i++) {
			if (devices[i].valid && devices[i].id == msg->device_id) {
				entry = &devices[i];
				break;
			}
			if (!devices[i].valid && entry == NULL) {
				entry = &devices[i];
			}
		}
		if (entry == NULL) {
			return false;
		}
		*entry = (struct proxy_device_info){msg->device_id, *info, true};
		if (cmd == IPC_DEVICE_GET_INFO_NO_ARRAYS) {
			return true;
		}
		return forward_metadata_block(native_fd, wine_fd, info->input_count, sizeof(enum xrt_input_name)) &&
		       forward_metadata_block(native_fd, wine_fd, info->output_count, sizeof(enum xrt_output_name)) &&
		       forward_metadata_block(native_fd, wine_fd, info->binding_profile_count,
		                              sizeof(struct ipc_binding_profile_info)) &&
		       forward_metadata_block(native_fd, wine_fd, info->total_input_pair_count,
		                              sizeof(struct xrt_binding_input_pair)) &&
		       forward_metadata_block(native_fd, wine_fd, info->total_output_pair_count,
		                              sizeof(struct xrt_binding_output_pair));
	}
	if (cmd == IPC_DEVICE_UPDATE_INPUT) {
		const struct ipc_device_update_input_msg *msg = request;
		struct mwxr_ipc_result_reply reply;
		if (!recv_exact(native_fd, &reply, sizeof(reply)) ||
		    !send_framed_reply(wine_fd, &reply, sizeof(reply))) {
			return false;
		}
		for (uint32_t i = 0; i < XRT_SYSTEM_MAX_DEVICES; i++) {
			if (devices[i].valid && devices[i].id == msg->id) {
				return forward_metadata_block(native_fd, wine_fd, devices[i].info.input_count,
				                              sizeof(struct xrt_input)) &&
				       forward_metadata_block(native_fd, wine_fd, devices[i].info.output_count,
				                              sizeof(struct xrt_output));
			}
		}
		return false;
	}
	if (cmd == IPC_DEVICE_GET_VIEW_POSES) {
		struct ipc_device_get_view_poses_reply reply;
		if (!recv_exact(native_fd, &reply, sizeof(reply)) ||
		    !send_framed_reply(wine_fd, &reply, sizeof(reply))) {
			return false;
		}
		if (reply.result != XRT_SUCCESS) {
			return true;
		}
		return forward_metadata_block(native_fd, wine_fd, reply.view_count, sizeof(struct xrt_fov)) &&
		       forward_metadata_block(native_fd, wine_fd, reply.view_count, sizeof(struct xrt_pose));
	}
	if (cmd == IPC_DEVICE_GET_DISTORTION_MESH) {
		struct ipc_device_get_distortion_mesh_reply reply;
		if (!recv_exact(native_fd, &reply, sizeof(reply)) ||
		    !send_framed_reply(wine_fd, &reply, sizeof(reply))) {
			return false;
		}
		if (reply.result != XRT_SUCCESS) {
			return true;
		}
		return forward_metadata_block(native_fd, wine_fd, reply.info.vertex_count, reply.info.stride) &&
		       forward_metadata_block(native_fd, wine_fd, reply.info.index_count_total, sizeof(int));
	}
	if (cmd == IPC_DEVICE_GET_DISTORTION_GRID || cmd == IPC_DEVICE_GET_VISIBILITY_MASK) {
		// Both replies are a result followed by a byte count.
		struct ipc_device_get_distortion_grid_reply reply;
		if (!recv_exact(native_fd, &reply, sizeof(reply)) ||
		    !send_framed_reply(wine_fd, &reply, sizeof(reply))) {
			return false;
		}
		return reply.result != XRT_SUCCESS || forward_metadata_block(native_fd, wine_fd, reply.grid_size, 1);
	}
	if (cmd == IPC_DEVICE_GET_PLANE_DETECTIONS_EXT) {
		struct ipc_device_get_plane_detections_ext_reply reply;
		if (!recv_exact(native_fd, &reply, sizeof(reply)) ||
		    !send_framed_reply(wine_fd, &reply, sizeof(reply))) {
			return false;
		}
		if (reply.result != XRT_SUCCESS) {
			return true;
		}
		return forward_metadata_block(native_fd, wine_fd, reply.location_size,
		                              sizeof(struct xrt_plane_detector_location_ext)) &&
		       forward_metadata_block(native_fd, wine_fd, reply.location_size, sizeof(uint32_t)) &&
		       forward_metadata_block(native_fd, wine_fd, reply.polygon_size,
		                              sizeof(struct xrt_plane_polygon_info_ext)) &&
		       forward_metadata_block(native_fd, wine_fd, reply.vertex_size, sizeof(struct xrt_vec2));
	}
	if (cmd == IPC_SPACE_LOCATE_SPACES || cmd == IPC_DEVICE_SET_HAPTIC_OUTPUT) {
		struct mwxr_ipc_result_reply allocation;
		if (!recv_exact(native_fd, &allocation, sizeof(allocation)) ||
		    !send_framed_reply(wine_fd, &allocation, sizeof(allocation))) {
			return false;
		}
		if (allocation.result != XRT_SUCCESS) {
			return true;
		}
		uint32_t count = cmd == IPC_SPACE_LOCATE_SPACES
		                     ? ((const struct ipc_space_locate_spaces_msg *)request)->space_count
		                     : ((const struct ipc_device_set_haptic_output_msg *)request)->samples.num_samples;
		uint64_t strides[2] = {cmd == IPC_SPACE_LOCATE_SPACES ? sizeof(uint32_t) : sizeof(float),
		                       sizeof(struct xrt_pose)};
		for (unsigned i = 0; i < (cmd == IPC_SPACE_LOCATE_SPACES ? 2u : 1u); i++) {
			if (count > MWXR_MAX_METADATA_BYTES / strides[i]) {
				return false;
			}
			size_t size = count * strides[i];
			void *data = malloc(size ? size : 1);
			if (data == NULL) {
				return false;
			}
			bool ok = recv_exact(wine_fd, data, size) && send_exact(native_fd, data, size);
			free(data);
			if (!ok) {
				return false;
			}
		}
		return forward_metadata_block(native_fd, wine_fd, cmd == IPC_SPACE_LOCATE_SPACES ? count : 1,
		                              cmd == IPC_SPACE_LOCATE_SPACES ? sizeof(struct xrt_space_relation)
		                                                             : sizeof(uint32_t));
	}
	fprintf(stderr, "proxy: unsupported variable-length command %u\n", cmd);
	return false;
}

struct proxy_layers
{
	struct ipc_layer_slot upload;
	uint32_t received;
	uint32_t total;
	uint32_t slot_id;
};

static xrt_result_t
stage_layers(struct proxy_layers *layers, const struct ipc_compositor_layer_copy_chunk_msg *msg)
{
	if (msg->chunk.size == 0 || msg->chunk.size > mwxr_IPC_LAYER_COPY_CHUNK_SIZE || msg->total_size == 0 ||
	    msg->total_size > sizeof(layers->upload)) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	if (msg->offset == 0) {
		memset(&layers->upload, 0, sizeof(layers->upload));
		layers->received = 0;
		layers->total = msg->total_size;
	}
	if (msg->total_size != layers->total || msg->offset != layers->received ||
	    msg->chunk.size > layers->total - layers->received) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	memcpy((uint8_t *)&layers->upload + layers->received, msg->chunk.data, msg->chunk.size);
	layers->received += msg->chunk.size;
	return XRT_SUCCESS;
}

static bool
commit_layers(int native_fd,
              struct ipc_shared_memory *ism,
              struct proxy_layers *layers,
              uint32_t size,
              bool semaphore,
              uint32_t semaphore_id,
              uint64_t semaphore_value,
              struct ipc_compositor_layer_sync_reply *reply)
{
	if (size != layers->total || size != layers->received || size == 0 ||
	    layers->upload.layer_count > IPC_MAX_LAYERS || layers->slot_id >= IPC_MAX_SLOTS ||
	    size !=
	        offsetof(struct ipc_layer_slot, layers) + layers->upload.layer_count * sizeof(struct ipc_layer_entry)) {
		reply->result = XRT_ERROR_INVALID_ARGUMENT;
		return true;
	}
	ism->slots[layers->slot_id] = layers->upload;
	if (semaphore) {
		struct ipc_compositor_layer_sync_with_semaphore_msg msg = {
		    .cmd = NATIVE_IPC_COMPOSITOR_LAYER_SYNC_WITH_SEMAPHORE,
		    .slot_id = layers->slot_id,
		    .semaphore_id = semaphore_id,
		    .semaphore_value = semaphore_value};
		if (!send_exact(native_fd, &msg, sizeof(msg))) {
			return false;
		}
	} else {
		struct ipc_compositor_layer_sync_msg msg = {
		    .cmd = NATIVE_IPC_COMPOSITOR_LAYER_SYNC, .slot_id = layers->slot_id, .handle_count = 0};
		struct mwxr_ipc_result_reply sync;
		uint32_t filler = NATIVE_IPC_COMPOSITOR_LAYER_SYNC;
		if (!send_exact(native_fd, &msg, sizeof(msg)) || !recv_exact(native_fd, &sync, sizeof(sync)) ||
		    sync.result != XRT_SUCCESS || !send_exact(native_fd, &filler, sizeof(filler))) {
			return false;
		}
	}
	if (!recv_exact(native_fd, reply, sizeof(*reply))) {
		return false;
	}
	if (reply->result == XRT_SUCCESS) {
		if (reply->free_slot_id >= IPC_MAX_SLOTS) {
			return false;
		}
		layers->slot_id = reply->free_slot_id;
		layers->received = layers->total = 0;
	}
	return true;
}

struct proxy_connection
{
	struct proxy_layers layers;
	struct proxy_device_info devices[XRT_SYSTEM_MAX_DEVICES];
	uint8_t request[MWXR_MAX_WIRE_MESSAGE];
	uint8_t translated[MWXR_MAX_WIRE_MESSAGE];
	uint8_t reply[MWXR_MAX_WIRE_MESSAGE];
};

static bool
proxy_client_messages(int wine_fd,
                      struct ipc_shared_memory *ism,
                      struct proxy_connection *connection,
                      int native_fd,
                      id<MTLDevice> device,
                      struct macos_wine_xr_monado_handoff *handoff)
{
	bool trace = getenv("MWXR_PROXY_TRACE") != NULL;
	struct proxy_device_info *devices = connection->devices;
	struct proxy_layers *layers = &connection->layers;
	uint8_t *request = connection->request;
	uint8_t *translated = connection->translated;
	uint8_t *reply = connection->reply;

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
		if (info.request_size < sizeof(cmd) || info.request_size > sizeof(connection->request) ||
		    info.reply_size > sizeof(connection->reply)) {
			fprintf(stderr, "proxy: unknown/oversized command %u\n", cmd);
			return false;
		}
		if (info.in_handles || info.out_handles) {
			fprintf(stderr, "proxy: rejecting native-handle command %u\n", cmd);
			return false;
		}

		if (trace) {
			fprintf(stderr, "proxy: Wine cmd=%u request=%zu reply=%zu\n", cmd, info.request_size,
			        info.reply_size);
		}
		memcpy(request, &cmd, sizeof(cmd));
		if (info.request_size > sizeof(cmd) &&
		    !recv_exact(wine_fd, request + sizeof(cmd), info.request_size - sizeof(cmd))) {
			return false;
		}

		if (cmd == IPC_INSTANCE_GET_SHM_CHUNK) {
			const struct ipc_instance_get_shm_chunk_msg *msg = (const void *)request;
			struct ipc_instance_get_shm_chunk_reply out = {.result = XRT_ERROR_INVALID_ARGUMENT};
			if (msg->offset < sizeof(*ism)) {
				size_t remaining = sizeof(*ism) - msg->offset;
				out.result = XRT_SUCCESS;
				out.chunk.size =
				    remaining < mwxr_IPC_SHM_COPY_CHUNK_SIZE ? remaining : mwxr_IPC_SHM_COPY_CHUNK_SIZE;
				memcpy(out.chunk.data, (uint8_t *)ism + msg->offset, out.chunk.size);
			}
			if (!send_framed_reply(wine_fd, &out, sizeof(out))) {
				return false;
			}
			continue;
		}
		if (cmd == IPC_COMPOSITOR_LAYER_COPY_CHUNK) {
			struct mwxr_ipc_result_reply out = {stage_layers(layers, (const void *)request)};
			if (!send_framed_reply(wine_fd, &out, sizeof(out))) {
				return false;
			}
			continue;
		}
		bool single = cmd == IPC_COMPOSITOR_LAYER_SYNC_SINGLE ||
		              cmd == IPC_COMPOSITOR_LAYER_SYNC_SINGLE_SEMAPHORE ||
		              cmd == IPC_COMPOSITOR_LAYER_SYNC_SINGLE_SEMAPHORE_ASYNC;
		bool chunk_commit = cmd == IPC_COMPOSITOR_LAYER_SYNC_COPY_COMMIT ||
		                    cmd == IPC_COMPOSITOR_LAYER_SYNC_COPY_COMMIT_SEMAPHORE;
		if (single || chunk_commit) {
			bool sem =
			    cmd != IPC_COMPOSITOR_LAYER_SYNC_SINGLE && cmd != IPC_COMPOSITOR_LAYER_SYNC_COPY_COMMIT;
			uint32_t size = 0, sem_id = 0;
			uint64_t value = 0;
			if (single) {
				const struct ipc_compositor_layer_sync_single_semaphore_msg *msg =
				    (const void *)request;
				size = msg->payload.size;
				memset(&layers->upload, 0, sizeof(layers->upload));
				layers->received = layers->total = 0;
				if (size <= mwxr_IPC_LAYER_SINGLE_PAYLOAD_SIZE && size <= sizeof(layers->upload)) {
					memcpy(&layers->upload, msg->payload.data, size);
					if (layers->upload.layer_count == 1) {
						layers->received = layers->total = size;
					}
				}
				if (sem) {
					sem_id = msg->semaphore_id;
					value = msg->semaphore_value;
				}
			} else {
				const struct ipc_compositor_layer_sync_copy_commit_semaphore_msg *msg =
				    (const void *)request;
				size = msg->total_size;
				if (sem) {
					sem_id = msg->semaphore_id;
					value = msg->semaphore_value;
				}
			}
			struct ipc_compositor_layer_sync_reply out = {0};
			if (!commit_layers(native_fd, ism, layers, size, sem, sem_id, value, &out)) {
				return false;
			}
			if (info.one_way) {
				if (out.result != XRT_SUCCESS) {
					return false;
				}
			} else if (!send_framed_reply(wine_fd, &out, sizeof(out))) {
				return false;
			}
			continue;
		}

		const uint8_t *native_request = request;
		size_t native_request_size = info.request_size;
		if (cmd == IPC_SWAPCHAIN_IMPORT_METAL_BOOTSTRAP) {
			if (!translate_texture_import(request, info.request_size, device, handoff, translated,
			                              sizeof(connection->translated), &native_request_size)) {
				fprintf(stderr, "proxy: texture bootstrap translation failed\n");
				return false;
			}
			native_request = translated;
		} else if (cmd == IPC_COMPOSITOR_SEMAPHORE_IMPORT_METAL_BOOTSTRAP) {
			if (!translate_semaphore_import(request, info.request_size, device, handoff, translated,
			                                sizeof(connection->translated), &native_request_size)) {
				fprintf(stderr, "proxy: semaphore bootstrap translation failed\n");
				return false;
			}
			native_request = translated;
		} else if (cmd == IPC_INSTANCE_DESCRIBE_CLIENT) {
			const struct ipc_instance_describe_client_msg *wine_msg = (const void *)request;
			struct __attribute__((packed))
			{
				uint32_t cmd;
				struct ipc_client_description desc;
			} native_msg = {0};
			native_msg.cmd = NATIVE_IPC_INSTANCE_DESCRIBE_CLIENT;
			native_msg.desc.pid = (pid_t)wine_msg->desc.pid;
			if ((int64_t)native_msg.desc.pid != wine_msg->desc.pid) {
				struct mwxr_ipc_result_reply invalid = {XRT_ERROR_INVALID_ARGUMENT};
				if (!send_framed_reply(wine_fd, &invalid, sizeof(invalid))) {
					return false;
				}
				continue;
			}
			native_msg.desc.info = wine_msg->desc.info;
			memcpy(translated, &native_msg, sizeof(native_msg));
			native_request = translated;
			native_request_size = sizeof(native_msg);
		} else if (cmd == IPC_SESSION_CREATE) {
			/*
			 * This was previously server policy for stream_socket clients.
			 * The compatibility proxy owns that policy now: the native Monado
			 * service sees an ordinary Unix-socket client.
			 */
			struct ipc_session_create_msg *msg = (struct ipc_session_create_msg *)request;
			msg->xsi.pacing_flags |= XRT_SESSION_PACING_USE_MIN_FRAME_PERIOD_BIT;
		}

		if (native_request == request) {
			uint32_t native_cmd = mwxr_native_command(cmd);
			if (native_cmd == 0) {
				return false;
			}
			memcpy(request, &native_cmd, sizeof(native_cmd));
		}
		if (!send_exact(native_fd, native_request, native_request_size)) {
			return false;
		}

		if (info.varlen || cmd == IPC_DEVICE_GET_INFO_NO_ARRAYS) {
			if (!forward_varlen(native_fd, wine_fd, cmd, request, devices)) {
				return false;
			}
			continue;
		}

		if (info.one_way) {
			continue;
		}
		if (cmd == IPC_SYSTEM_GET_CLIENT_INFO) {
			struct __attribute__((packed))
			{
				xrt_result_t result;
				struct ipc_app_state ias;
			} native_reply = {0};
			if (!recv_exact(native_fd, &native_reply, sizeof(native_reply))) {
				return false;
			}
			struct ipc_system_get_client_info_reply wine_reply = {0};
			wine_reply.result = native_reply.result;
			if (native_reply.result == XRT_SUCCESS) {
				struct ipc_app_state aligned_state;
				memcpy(&aligned_state, &native_reply.ias, sizeof(aligned_state));
				const struct ipc_app_state *state = &aligned_state;
				wine_reply.ias.id = state->id;
				wine_reply.ias.primary_application = state->primary_application;
				wine_reply.ias.session_active = state->session_active;
				wine_reply.ias.session_visible = state->session_visible;
				wine_reply.ias.session_focused = state->session_focused;
				wine_reply.ias.session_overlay = state->session_overlay;
				wine_reply.ias.io_blocks = state->io_blocks;
				wine_reply.ias.z_order = state->z_order;
				wine_reply.ias.pid = state->pid;
				wine_reply.ias.info = state->info;
			}
			if (!send_framed_reply(wine_fd, &wine_reply, sizeof(wine_reply))) {
				return false;
			}
			continue;
		}
		if (info.reply_size == 0 || !recv_exact(native_fd, reply, info.reply_size) ||
		    !send_framed_reply(wine_fd, reply, info.reply_size)) {
			return false;
		}
	}
}

static bool
proxy_one_client(int wine_fd, int native_fd, id<MTLDevice> device, struct macos_wine_xr_monado_handoff *handoff)
{
	struct ipc_shared_memory *ism = map_native_shm(native_fd);
	if (ism == NULL) {
		return false;
	}
	fprintf(stderr, "proxy: native-shm mapped; layer metadata uses native shared-memory slots\n");
	struct proxy_connection *connection = calloc(1, sizeof(*connection));
	bool ok = connection != NULL && proxy_client_messages(wine_fd, ism, connection, native_fd, device, handoff);
	free(connection);
	munmap(ism, sizeof(*ism));
	return ok;
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

	fprintf(stderr, "proxy: Monado headers revision=%s protocol-sha256=%s\n", MWXR_MONADO_REVISION,
	        MWXR_MONADO_PROTOCOL_SHA256);
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
