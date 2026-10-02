// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// Exercise the production proxy over real sockets, including native fd transfer.
#define main proxy_program_main
#include "../../src/proxy/monado_proxy.m"
#undef main
#include <pthread.h>
#include <assert.h>

#define CHECK(expr)                                                                                                    \
	do {                                                                                                           \
		if (!(expr)) {                                                                                         \
			fprintf(stderr, "failed line %d: %s\n", __LINE__, #expr);                                      \
			abort();                                                                                       \
		}                                                                                                      \
	} while (0)
struct test_context
{
	int fd;
	struct ipc_shared_memory *ism;
};
static void *
serve(void *arg)
{
	struct test_context *ctx = arg;
	int native[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, native) == 0);
	// A second thread acts as native Monado below.
	extern void *native_service(void *);
	struct test_context peer = {native[1], ctx->ism};
	pthread_t thread;
	CHECK(pthread_create(&thread, NULL, native_service, &peer) == 0);
	CHECK(proxy_one_client(ctx->fd, native[0], nil, NULL));
	close(native[0]);
	close(ctx->fd);
	CHECK(pthread_join(thread, NULL) == 0);
	close(native[1]);
	return NULL;
}
static int memory_fd;
void *
native_service(void *arg)
{
	struct test_context *ctx = arg;
	uint32_t cmd;
	CHECK(recv_exact(ctx->fd, &cmd, sizeof(cmd)));
	CHECK(cmd == NATIVE_IPC_INSTANCE_GET_SHM_FD);
	struct mwxr_ipc_result_reply result = {XRT_SUCCESS};
	char control[CMSG_SPACE(sizeof(int))] = {0};
	struct iovec iov = {&result, 1}; // Deliberately fragmented fd-bearing reply.
	struct msghdr msg = {0};
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);
	struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(c), &memory_fd, sizeof(int));
	CHECK(sendmsg(ctx->fd, &msg, 0) == 1);
	CHECK(send_exact(ctx->fd, (uint8_t *)&result + 1, sizeof(result) - 1));
	// Four metadata submissions: chunk/no fence, chunk/semaphore, single, async/single.
	for (unsigned i = 0; i < 4; i++) {
		CHECK(recv_exact(ctx->fd, &cmd, sizeof(cmd)));
		bool sem = i == 1 || i == 3;
		CHECK(cmd ==
		      (sem ? NATIVE_IPC_COMPOSITOR_LAYER_SYNC_WITH_SEMAPHORE : NATIVE_IPC_COMPOSITOR_LAYER_SYNC));
		uint32_t slot;
		CHECK(recv_exact(ctx->fd, &slot, sizeof(slot)));
		CHECK(slot == i);
		CHECK(ctx->ism->slots[slot].data.frame_id == 100 + i);
		CHECK(ctx->ism->slots[slot].layer_count == (i == 0 ? 2 : 1));
		if (sem) {
			uint32_t id;
			uint64_t value;
			CHECK(recv_exact(ctx->fd, &id, sizeof(id)) && id == 7);
			CHECK(recv_exact(ctx->fd, &value, sizeof(value)) && value == 42);
		} else {
			uint32_t count;
			CHECK(recv_exact(ctx->fd, &count, sizeof(count)) && count == 0);
			CHECK(send_exact(ctx->fd, &result, sizeof(result)));
			CHECK(recv_exact(ctx->fd, &cmd, sizeof(cmd)) && cmd == NATIVE_IPC_COMPOSITOR_LAYER_SYNC);
		}
		struct ipc_compositor_layer_sync_reply reply = {XRT_SUCCESS, (i + 1) % IPC_MAX_SLOTS};
		CHECK(send_exact(ctx->fd, &reply, sizeof(reply)));
	}
	// Variable-length native replies need one Wine frame per logical block.
	CHECK(recv_exact(ctx->fd, &cmd, sizeof(cmd)) && cmd == NATIVE_IPC_DEVICE_GET_INFO);
	uint32_t id;
	CHECK(recv_exact(ctx->fd, &id, sizeof(id)) && id == 17);
	struct ipc_device_get_info_reply info = {0};
	info.info.input_count = info.info.output_count = 1;
	CHECK(send_exact(ctx->fd, &info, sizeof(info)));
	enum xrt_input_name input = XRT_INPUT_GENERIC_HEAD_POSE;
	enum xrt_output_name output = XRT_OUTPUT_NAME_SIMPLE_VIBRATION;
	CHECK(send_exact(ctx->fd, &input, sizeof(input)) && send_exact(ctx->fd, &output, sizeof(output)));
	CHECK(recv_exact(ctx->fd, &cmd, sizeof(cmd)) && cmd == NATIVE_IPC_DEVICE_UPDATE_INPUT);
	CHECK(recv_exact(ctx->fd, &id, sizeof(id)) && id == 17);
	struct xrt_input state = {0};
	struct xrt_output out = {0};
	CHECK(send_exact(ctx->fd, &result, sizeof(result)) && send_exact(ctx->fd, &state, sizeof(state)) &&
	      send_exact(ctx->fd, &out, sizeof(out)));
	// A failed mesh query has no trailing data: the following reply must remain aligned.
	CHECK(recv_exact(ctx->fd, &cmd, sizeof(cmd)) && cmd == NATIVE_IPC_DEVICE_GET_DISTORTION_MESH);
	CHECK(recv_exact(ctx->fd, &id, sizeof(id)) && id == 17);
	struct ipc_device_get_distortion_mesh_reply mesh = {.result = XRT_ERROR_FEATURE_NOT_SUPPORTED};
	CHECK(send_exact(ctx->fd, &mesh, sizeof(mesh)));
	// Ordinary commands must use native IDs, even if Wine IDs diverge.
	CHECK(recv_exact(ctx->fd, &cmd, sizeof(cmd)) && cmd == NATIVE_IPC_INSTANCE_IS_SYSTEM_AVAILABLE);
	struct ipc_instance_is_system_available_reply available = {XRT_SUCCESS, true};
	CHECK(send_exact(ctx->fd, &available, sizeof(available)));
	CHECK(recv(ctx->fd, &cmd, sizeof(cmd), 0) == 0);
	return NULL;
}
static void
fragmented(int fd, const void *data, size_t size)
{
	for (size_t i = 0; i < size; i++) {
		CHECK(send_exact(fd, (const uint8_t *)data + i, 1));
	}
}
static void
get_reply(int fd, void *data, size_t size)
{
	uint32_t count;
	CHECK(recv_exact(fd, &count, sizeof(count)) && count == size);
	CHECK(recv_exact(fd, data, size));
}
int
main(void)
{
	char path[] = "/tmp/mwxr-proxy-shm-XXXXXX";
	memory_fd = mkstemp(path);
	CHECK(memory_fd >= 0);
	unlink(path);
	CHECK(ftruncate(memory_fd, sizeof(struct ipc_shared_memory)) == 0);
	struct ipc_shared_memory *ism = mmap(NULL, sizeof(*ism), PROT_READ | PROT_WRITE, MAP_SHARED, memory_fd, 0);
	CHECK(ism != MAP_FAILED);
	int wine[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, wine) == 0);
	struct test_context ctx = {wine[1], ism};
	pthread_t thread;
	CHECK(pthread_create(&thread, NULL, serve, &ctx) == 0);
	struct ipc_instance_get_shm_chunk_msg shm = {IPC_INSTANCE_GET_SHM_CHUNK, sizeof(*ism) - 3};
	fragmented(wine[0], &shm, sizeof(shm));
	struct ipc_instance_get_shm_chunk_reply shm_reply;
	get_reply(wine[0], &shm_reply, sizeof(shm_reply));
	CHECK(shm_reply.result == XRT_SUCCESS && shm_reply.chunk.size == 3);
	shm.offset = sizeof(*ism);
	fragmented(wine[0], &shm, sizeof(shm));
	get_reply(wine[0], &shm_reply, sizeof(shm_reply));
	CHECK(shm_reply.result == XRT_ERROR_INVALID_ARGUMENT);
	// Reject malformed chunks locally without sending native messages.
	struct ipc_compositor_layer_copy_chunk_msg bad = {.cmd = IPC_COMPOSITOR_LAYER_COPY_CHUNK};
	fragmented(wine[0], &bad, sizeof(bad));
	struct mwxr_ipc_result_reply result;
	get_reply(wine[0], &result, sizeof(result));
	CHECK(result.result == XRT_ERROR_INVALID_ARGUMENT);
	for (unsigned i = 0; i < 4; i++) {
		struct ipc_layer_slot slot = {0};
		slot.data.frame_id = 100 + i;
		slot.layer_count = i == 0 ? 2 : 1;
		uint32_t size =
		    offsetof(struct ipc_layer_slot, layers) + slot.layer_count * sizeof(struct ipc_layer_entry);
		if (i < 2) {
			for (uint32_t offset = 0; offset < size;) {
				struct ipc_compositor_layer_copy_chunk_msg chunk = {
				    .cmd = IPC_COMPOSITOR_LAYER_COPY_CHUNK, .offset = offset, .total_size = size};
				chunk.chunk.size = size - offset < mwxr_IPC_LAYER_COPY_CHUNK_SIZE
				                       ? size - offset
				                       : mwxr_IPC_LAYER_COPY_CHUNK_SIZE;
				memcpy(chunk.chunk.data, (uint8_t *)&slot + offset, chunk.chunk.size);
				fragmented(wine[0], &chunk, sizeof(chunk));
				get_reply(wine[0], &result, sizeof(result));
				CHECK(result.result == XRT_SUCCESS);
				offset += chunk.chunk.size;
			}
			if (i == 0) {
				struct ipc_compositor_layer_sync_copy_commit_msg commit = {
				    IPC_COMPOSITOR_LAYER_SYNC_COPY_COMMIT, size};
				fragmented(wine[0], &commit, sizeof(commit));
			} else {
				struct ipc_compositor_layer_sync_copy_commit_semaphore_msg commit = {
				    IPC_COMPOSITOR_LAYER_SYNC_COPY_COMMIT_SEMAPHORE, size, 7, 42};
				fragmented(wine[0], &commit, sizeof(commit));
			}
		} else {
			struct ipc_compositor_layer_sync_single_semaphore_msg single = {
			    .cmd = i == 2 ? IPC_COMPOSITOR_LAYER_SYNC_SINGLE
			                  : IPC_COMPOSITOR_LAYER_SYNC_SINGLE_SEMAPHORE_ASYNC,
			    .semaphore_id = 7,
			    .semaphore_value = 42};
			single.payload.size = size;
			memcpy(single.payload.data, &slot, size);
			fragmented(wine[0], &single,
			           i == 2 ? sizeof(struct ipc_compositor_layer_sync_single_msg) : sizeof(single));
		}
		if (i != 3) {
			struct ipc_compositor_layer_sync_reply reply;
			get_reply(wine[0], &reply, sizeof(reply));
			CHECK(reply.result == XRT_SUCCESS && reply.free_slot_id == i + 1);
		}
	}
	struct ipc_device_get_info_msg query = {IPC_DEVICE_GET_INFO, 17};
	fragmented(wine[0], &query, sizeof(query));
	struct ipc_device_get_info_reply info;
	get_reply(wine[0], &info, sizeof(info));
	CHECK(info.result == XRT_SUCCESS && info.info.input_count == 1 && info.info.output_count == 1);
	enum xrt_input_name input;
	enum xrt_output_name output;
	get_reply(wine[0], &input, sizeof(input));
	get_reply(wine[0], &output, sizeof(output));
	CHECK(input == XRT_INPUT_GENERIC_HEAD_POSE && output == XRT_OUTPUT_NAME_SIMPLE_VIBRATION);
	struct ipc_device_update_input_msg update = {IPC_DEVICE_UPDATE_INPUT, 17};
	fragmented(wine[0], &update, sizeof(update));
	get_reply(wine[0], &result, sizeof(result));
	struct xrt_input state;
	struct xrt_output out;
	get_reply(wine[0], &state, sizeof(state));
	get_reply(wine[0], &out, sizeof(out));
	struct ipc_device_get_distortion_mesh_msg mesh_query = {IPC_DEVICE_GET_DISTORTION_MESH, 17};
	fragmented(wine[0], &mesh_query, sizeof(mesh_query));
	struct ipc_device_get_distortion_mesh_reply mesh;
	get_reply(wine[0], &mesh, sizeof(mesh));
	CHECK(mesh.result == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	uint32_t cmd = IPC_INSTANCE_IS_SYSTEM_AVAILABLE;
	fragmented(wine[0], &cmd, sizeof(cmd));
	struct ipc_instance_is_system_available_reply available;
	get_reply(wine[0], &available, sizeof(available));
	CHECK(available.result == XRT_SUCCESS && available.available);
	close(wine[0]);
	CHECK(pthread_join(thread, NULL) == 0);
	munmap(ism, sizeof(*ism));
	close(memory_fd);
	puts(
	    "proxy protocol: fd mapping, local chunks, native slots/fences, async ordering and fragmented Wine "
	    "messages passed");
	return 0;
}
