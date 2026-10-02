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
