// Copyright 2020-2024 Collabora, Ltd.
// Copyright 2025-2026, NVIDIA CORPORATION.
// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#define mwxr_IPC_METAL_BOOTSTRAP_NAME_SIZE 64
struct mwxr_ipc_metal_bootstrap_name
{
	char name[mwxr_IPC_METAL_BOOTSTRAP_NAME_SIZE];
};

/*!
 * Native Metal array-texture transport. Each name identifies the Mach port of an
 * existing MTLSharedTextureHandle registered by an external producer in the user's bootstrap
 * namespace. The service reopens the same storage; no pixel copy is involved.
 */
struct mwxr_ipc_arg_swapchain_metal_bootstrap
{
	uint32_t image_count;
	struct mwxr_ipc_metal_bootstrap_name names[XRT_MAX_SWAPCHAIN_IMAGES];
};

/*!
 * Bounded byte chunk used to copy the large ipc_shared_memory structure to
 * byte-stream clients without placing the whole structure in a generated IPC reply
 * on the Windows thread stack.
 */
#define mwxr_IPC_SHM_COPY_CHUNK_SIZE 4096
struct mwxr_ipc_shm_copy_chunk
{
	uint32_t size;
	uint8_t data[mwxr_IPC_SHM_COPY_CHUNK_SIZE];
};

/*!
 * Bounded active-layer upload chunk for byte-stream compositor transports.
 * Keep the generated command comfortably below IPC_BUF_SIZE (2048).
 */
#define mwxr_IPC_LAYER_COPY_CHUNK_SIZE 1900
struct mwxr_ipc_layer_copy_chunk
{
	uint32_t size;
	uint8_t data[mwxr_IPC_LAYER_COPY_CHUNK_SIZE];
};

/*!
 * Fast path for the overwhelmingly common one-layer frame. The byte array is
 * fixed-width on the wire, while size describes the active native prefix:
 * frame data + layer_count + one ipc_layer_entry.
 */
#define mwxr_IPC_LAYER_SINGLE_PAYLOAD_SIZE 4000
struct mwxr_ipc_layer_single_payload
{
	uint32_t size;
	uint8_t data[mwxr_IPC_LAYER_SINGLE_PAYLOAD_SIZE];
};

// Frozen Wine aggregates: native Monado is free to use platform pid_t.
struct mwxr_ipc_client_description
{
	int64_t pid;
	struct xrt_application_info info;
};
struct mwxr_ipc_app_state
{
	uint32_t id;
	bool primary_application;
	bool session_active;
	bool session_visible;
	bool session_focused;
	bool session_overlay;
	struct ipc_client_io_blocks io_blocks;
	uint32_t z_order;
	int64_t pid;
	struct xrt_application_info info;
};

_Static_assert(offsetof(struct mwxr_ipc_client_description, info) == 8, "Frozen Wine description layout");
_Static_assert(offsetof(struct mwxr_ipc_app_state, pid) == 24, "Frozen Wine app-state PID layout");
_Static_assert(offsetof(struct mwxr_ipc_app_state, info) == 32, "Frozen Wine app-state info layout");
