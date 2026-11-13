// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// PE graphics wrappers follow Proton's manual entry-point model. The graphics
// adaptation differs on macOS: a GraphicsInterop backend exposes native
// runtime-owned Metal objects as D3D11 resources. Nothing here is specific to
// the D3D translation layer in use.
#include "openxr_loader.h"
#include "mndx_display_distortion.h"
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include "graphics_interop.h"
#include "graphics_vulkan.h"
struct SwapState { wine_XrSwapchain *wrapper; GraphicsImage *images; XrSwapchainImageMetalKHR *metal; uint32_t count;
    uint32_t *acquired, acquired_head, acquired_count; SwapState *next;
    bool waited, foveation_applied, foveation_failed, requires_foveation_apply; uint64_t foveation_map; };
struct SessionState { ID3D11Device *device; GraphicsInterop *interop; SwapState *swapchains;
    CRITICAL_SECTION release_lock;
    // MWXR_GAZE_FOVEATION: the packed stereo layout last submitted, and the map now set.
    XrSwapchain gaze_swapchain; XrRect2Di gaze_rects[2]; XrFovf gaze_fovs[2]; uint64_t gaze_map; };
static wine_XrSession *active_session;
static XrResult call_result(NTSTATUS status, XrResult result) { return status ? XR_ERROR_RUNTIME_FAILURE : result; }
static int32_t native_graphics_call(void *params) { return UNIX_CALL(mw_graphics_native, params); }
static const GraphicsNativeHost native_host = {native_graphics_call};
extern "C" XrResult WINAPI xrGetD3D11GraphicsRequirementsKHR(XrInstance instance, XrSystemId system,
                                                           XrGraphicsRequirementsD3D11KHR *requirements)
{
    if (!requirements || requirements->type != XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR || requirements->next)
        return XR_ERROR_VALIDATION_FAILURE;
    struct xrGetD3D11GraphicsRequirementsKHR_params params = {instance, system, requirements};
    NTSTATUS status = UNIX_CALL(xrGetD3D11GraphicsRequirementsKHR, &params);
    XrResult result = call_result(status, params.result);
    if (XR_FAILED(result)) return result;
    // The translation layer's default hardware adapter must match the native
    // Metal device at session creation; that check is mandatory, not a fallback.
    IDXGIFactory1 *factory = nullptr; IDXGIAdapter1 *adapter = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory);
    if (SUCCEEDED(hr)) hr = factory->EnumAdapters1(0, &adapter);
    DXGI_ADAPTER_DESC1 desc = {};
    if (SUCCEEDED(hr)) hr = adapter->GetDesc1(&desc);
    if (adapter) adapter->Release(); if (factory) factory->Release();
    if (FAILED(hr)) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    requirements->adapterLuid = desc.AdapterLuid;
    requirements->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}
static void destroy_state(SessionState *state)
{
    DeleteCriticalSection(&state->release_lock);
    delete state->interop;
    if (state->device) state->device->Release();
    free(state);
}
extern "C" XrResult WINAPI xrCreateSession(XrInstance instance, const XrSessionCreateInfo *info, XrSession *session)
{
    if (!info || !session || info->type != XR_TYPE_SESSION_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    *session = XR_NULL_HANDLE;
    if (active_session) return XR_ERROR_LIMIT_REACHED;
    wine_XrInstance *parent = wine_instance_from_handle(instance);
    const auto *base_binding = (const XrBaseInStructure *)info->next;
    if (base_binding && base_binding->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
        XrResult result = mw_vk_create_session(instance, info, session);
        if (XR_SUCCEEDED(result)) active_session = wine_session_from_handle(*session);
        return result;
    }
    if (!parent->d3d11_enabled) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    const auto *binding = (const XrGraphicsBindingD3D11KHR *)info->next;
    if (!binding || binding->type != XR_TYPE_GRAPHICS_BINDING_D3D11_KHR || binding->next || !binding->device)
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    auto *wrapper = (wine_XrSession *)calloc(1, sizeof(wine_XrSession));
    auto *state = (SessionState *)calloc(1, sizeof(SessionState));
    if (!wrapper || !state) { free(wrapper); free(state); return XR_ERROR_OUT_OF_MEMORY; }
    InitializeCriticalSection(&state->release_lock);
    state->device = binding->device; state->device->AddRef();
    HRESULT hr = mw_graphics_interop_open(state->device, native_host, &state->interop);
    if (FAILED(hr)) {
        fprintf(stderr, "wineopenxr: ERROR no graphics interop backend for this D3D11 device hr=%#lx\n", hr);
        destroy_state(state); free(wrapper); return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    if (parent->foveation_d3d11_enabled && !state->interop->supports_target_foveation()) {
        destroy_state(state); free(wrapper); return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    wrapper->instance = parent; wrapper->metal_device = state->interop->metal_device(); wrapper->graphics = state;
    struct xrCreateSession_params params = {};
    params.instance = instance; params.createInfo = info; params.session = &wrapper->host_session; params.wine_session = wrapper;
    NTSTATUS status = UNIX_CALL(xrCreateSession, &params);
    XrResult result = call_result(status, params.result);
    if (XR_SUCCEEDED(result)) {
        hr = state->interop->bind_completion_event(wrapper->metal_event);
        if (SUCCEEDED(hr))
        {
            char caps[256];
            mw_graphics_format_caps(state->interop->capabilities(), caps, sizeof(caps));
            fprintf(stderr, "wineopenxr: session backend=%s api=D3D11 sync=%s caps=%s\n", state->interop->name(),
                    mw_graphics_sync_name(state->interop->sync()), caps);
        }
        else {
            fprintf(stderr, "wineopenxr: ERROR %s completion-event binding failed hr=%#lx\n", state->interop->name(), hr);
            struct xrDestroySession_params rollback = {(XrSession)wrapper};
            UNIX_CALL(xrDestroySession, &rollback);
            result = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }
    }
    if (XR_FAILED(result)) { destroy_state(state); free(wrapper); return result; }
    active_session = wrapper; *session = (XrSession)wrapper;
    return result;
}
extern "C" XrResult WINAPI xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo *info, XrSwapchain *swapchain)
{
    if (wine_session_from_handle(session)->vulkan) return mw_vk_create_swapchain(session, info, swapchain);
    if (!info || !swapchain || info->type != XR_TYPE_SWAPCHAIN_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    *swapchain = XR_NULL_HANDLE;
    if (info->next && wine_session_from_handle(session)->instance->foveation_d3d11_enabled && info->arraySize != 1)
        return XR_ERROR_FEATURE_UNSUPPORTED;
    auto *wrapper = (wine_XrSwapchain *)calloc(1, sizeof(wine_XrSwapchain));
    auto *state = (SwapState *)calloc(1, sizeof(SwapState));
    if (!wrapper || !state) { free(wrapper); free(state); return XR_ERROR_OUT_OF_MEMORY; }
    wrapper->session = wine_session_from_handle(session); wrapper->info = *info; wrapper->graphics = state; state->wrapper = wrapper;
    // The app keeps its own create info; native images may need backend usage.
    XrSwapchainCreateInfo native = *info;
    native.usageFlags |= ((SessionState *)wrapper->session->graphics)->interop->extra_swapchain_usage();
    struct xrCreateSwapchain_params params = {session, &native, &wrapper->host_swapchain};
    NTSTATUS status = UNIX_CALL(xrCreateSwapchain, &params);
    XrResult result = call_result(status, params.result);
    if (XR_FAILED(result)) { free(state); free(wrapper); return result; }
    state->requires_foveation_apply = info->next && wrapper->session->instance->foveation_d3d11_enabled;
    auto *parent = (SessionState *)wrapper->session->graphics;
    state->next = parent->swapchains; parent->swapchains = state;
    *swapchain = (XrSwapchain)wrapper;
    return result;
}
extern "C" XrResult WINAPI xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t capacity,
    uint32_t *count, XrSwapchainImageBaseHeader *images)
{
    if (wine_swapchain_from_handle(swapchain)->session->vulkan) return mw_vk_enumerate_images(swapchain, capacity, count, images);
    if (!count || (capacity && !images)) return XR_ERROR_VALIDATION_FAILURE;
    wine_XrSwapchain *wrapper = wine_swapchain_from_handle(swapchain);
    auto *state = (SwapState *)wrapper->graphics;
    auto *session = (SessionState *)wrapper->session->graphics;
    if (!state->images) {
        struct xrEnumerateSwapchainImages_params params = {swapchain, 0, &state->count, nullptr};
        NTSTATUS status = UNIX_CALL(xrEnumerateSwapchainImages, &params);
        XrResult result = call_result(status, params.result);
        if (XR_FAILED(result)) return result;
        if (capacity && capacity < state->count) { *count = state->count; return XR_ERROR_SIZE_INSUFFICIENT; }
        if (!capacity) { *count = state->count; return result; }
        auto *metal = (XrSwapchainImageMetalKHR *)calloc(state->count, sizeof(XrSwapchainImageMetalKHR));
        auto *images = (GraphicsImage *)calloc(state->count, sizeof(GraphicsImage));
        auto *acquired = (uint32_t *)calloc(state->count, sizeof(uint32_t));
        if (!metal || !images || !acquired) { free(metal); free(images); free(acquired); return XR_ERROR_OUT_OF_MEMORY; }
        for (uint32_t i = 0; i < state->count; ++i) metal[i].type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
        params.imageCapacityInput = state->count; params.images = (XrSwapchainImageBaseHeader *)metal;
        status = UNIX_CALL(xrEnumerateSwapchainImages, &params); result = call_result(status, params.result);
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = wrapper->info.width; desc.Height = wrapper->info.height; desc.ArraySize = wrapper->info.arraySize;
        desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Format = (DXGI_FORMAT)wrapper->info.format;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        GraphicsInterop *interop = session->interop;
        uint32_t zero_copy = 0, iosurface = 0;
        if (XR_SUCCEEDED(result)) for (uint32_t i = 0; i < state->count; ++i) {
            HRESULT hr = interop->import_image(metal[i].texture, desc, &images[i]);
            if (FAILED(hr)) {
                fprintf(stderr, "wineopenxr: ERROR %s import rejected image=%u Metal=%p hr=%#lx; no implicit copy\n",
                        interop->name(), i, metal[i].texture, hr);
                result = XR_ERROR_RUNTIME_FAILURE; break;
            }
            zero_copy += images[i].zero_copy; iosurface += images[i].iosurface;
            fprintf(stderr, "wineopenxr: %s image=%u Metal=%p D3D11=%p array=%u\n", images[i].zero_copy ? "zero-copy" : "COPY",
                    i, metal[i].texture, images[i].texture, desc.ArraySize);
        }
        if (XR_FAILED(result)) {
            for (uint32_t i = 0; i < state->count; ++i) interop->release_image(&images[i]);
            free(metal); free(images); free(acquired); return result;
        }
        // One summary per swapchain; release paths never log per frame.
        fprintf(stderr, "wineopenxr: swapchain=%p backend=%s api=D3D11 images=%u %ux%u array=%u format=%u "
                "native=MTLTexture iosurface=%u/%u sync=%s zero-copy=%s%s\n", (void *)wrapper, interop->name(),
                state->count, desc.Width, desc.Height, desc.ArraySize, (unsigned)desc.Format, iosurface, state->count,
                mw_graphics_sync_name(interop->sync()), zero_copy == state->count ? "yes" : "no",
                zero_copy == state->count ? "" : " fallback=explicit-gpu-blit");
        state->images = images; state->metal = metal; state->acquired = acquired;
    }
    *count = state->count;
    if (!capacity) return XR_SUCCESS;
    if (capacity < state->count) return XR_ERROR_SIZE_INSUFFICIENT;
    auto *d3d = (XrSwapchainImageD3D11KHR *)images;
    for (uint32_t i = 0; i < state->count; ++i) {
        if (d3d[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR || d3d[i].next) return XR_ERROR_VALIDATION_FAILURE;
        d3d[i].texture = state->images[i].texture;
    }
    return XR_SUCCESS;
}
// Test diagnostic, deliberately outside OpenXR GIPA. Returns borrowed runtime
// objects only while the caller keeps the swapchain alive and owns this image.
extern "C" __declspec(dllexport) XrResult WINAPI MWXRProbeSwapchainImage(
    XrSwapchain swapchain, uint32_t index, UINT64 *device, UINT64 *texture)
{
    if (!swapchain || !device || !texture) return XR_ERROR_VALIDATION_FAILURE;
    auto *wrapper = wine_swapchain_from_handle(swapchain);
    if (wrapper->session->vulkan) return XR_ERROR_FUNCTION_UNSUPPORTED;
    auto *state = (SwapState *)wrapper->graphics;
    if (!state->metal || index >= state->count) return XR_ERROR_INDEX_OUT_OF_RANGE;
    *device = (UINT64)wrapper->session->metal_device;
    *texture = (UINT64)state->metal[index].texture;
    return XR_SUCCESS;
}
extern "C" XrResult WINAPI xrDestroySwapchain(XrSwapchain swapchain)
{
    if (wine_swapchain_from_handle(swapchain)->session->vulkan) return mw_vk_destroy_swapchain(swapchain);
    wine_XrSwapchain *wrapper = wine_swapchain_from_handle(swapchain);
    auto *state = (SwapState *)wrapper->graphics;
    struct xrDestroySwapchain_params params = {swapchain};
    NTSTATUS status = UNIX_CALL(xrDestroySwapchain, &params);
    XrResult result = call_result(status, params.result);
    if (XR_FAILED(result)) return result;
    auto *parent = (SessionState *)wrapper->session->graphics;
    if (swapchain == parent->gaze_swapchain) {
        parent->gaze_swapchain = XR_NULL_HANDLE;
        if (parent->gaze_map) { parent->interop->set_frame_rate_map(nullptr); parent->gaze_map = 0; }
    }
    SwapState **link = &parent->swapchains;
    while (*link && *link != state) link = &(*link)->next;
    if (*link) *link = state->next;
    for (uint32_t i = 0; state->images && i < state->count; ++i) parent->interop->release_image(&state->images[i]);
    free(state->metal); free(state->images); free(state->acquired); free(state); free(wrapper);
    return result;
}
extern "C" XrResult WINAPI xrDestroySession(XrSession session)
{
    if (wine_session_from_handle(session)->vulkan) {
        XrResult result = mw_vk_destroy_session(session);
        if (XR_SUCCEEDED(result)) active_session = nullptr;
        return result;
    }
    auto *wrapper = wine_session_from_handle(session);
    auto *state = (SessionState *)wrapper->graphics;
    while (state->swapchains) {
        XrResult result = xrDestroySwapchain((XrSwapchain)state->swapchains->wrapper);
        if (XR_FAILED(result)) return result;
    }
    state->interop->flush();
    struct xrDestroySession_params params = {session};
    NTSTATUS status = UNIX_CALL(xrDestroySession, &params);
    XrResult result = call_result(status, params.result);
    if (XR_SUCCEEDED(result)) { destroy_state(state); active_session = nullptr; free(wrapper); }
    return result;
}
extern "C" XrResult mw_pe_cleanup_instance(wine_XrInstance *instance)
{
    if (active_session && active_session->instance == instance) return xrDestroySession((XrSession)active_session);
    return XR_SUCCESS;
}
// Acquired indices are released in FIFO order; only a copy fallback needs them.
extern "C" XrResult WINAPI xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo *info,
                                                   uint32_t *index)
{
    if (wine_swapchain_from_handle(swapchain)->session->vulkan) return mw_vk_acquire(swapchain, info, index);
    auto *wrapper = wine_swapchain_from_handle(swapchain);
    auto *state = (SwapState *)wrapper->graphics;
    struct xrAcquireSwapchainImage_params params = {swapchain, info, index};
    NTSTATUS status = UNIX_CALL(xrAcquireSwapchainImage, &params);
    XrResult result = call_result(status, params.result);
    if (XR_SUCCEEDED(result) && state->acquired && state->acquired_count < state->count)
        state->acquired[(state->acquired_head + state->acquired_count++) % state->count] = *index;
    // Once per frame, before the application records the passes for this image.
    auto *session = (SessionState *)wrapper->session->graphics;
    if (XR_SUCCEEDED(result) && wrapper->session->instance->gaze_foveation && swapchain == session->gaze_swapchain) {
        mw_gaze_foveation_params gaze = {swapchain, wrapper->info.width, wrapper->info.height,
            {session->gaze_rects[0], session->gaze_rects[1]}, {session->gaze_fovs[0], session->gaze_fovs[1]}, 0, XR_SUCCESS};
        // An application that copies its own eye textures into the swapchain (OpenVR through
        // xrizer) renders at that texture's size, which need not be the swapchain's: a render
        // scale, both eyes in one texture, or part of a larger texture under dynamic
        // resolution. DXMT leaves the latest copy's source on the image (patch 0022).
        uint32_t source[2][8] = {};
        for (uint32_t i = 0; state->images && i < state->count; ++i) {
            for (unsigned slice = 0; slice < 2; ++slice) {
                GUID guid = {0x6d777872, 0x6373, 0x4f56, {0x90, 0x01, 'c', 'o', 'p', 'y', 0, (BYTE)slice}};
                uint32_t found[8] = {}; UINT size = sizeof(found);
                auto *texture = (ID3D11Texture2D *)state->images[i].texture;
                if (texture && SUCCEEDED(texture->GetPrivateData(guid, &size, found)) && size == sizeof(found) &&
                    found[0] > source[slice][0])
                    memcpy(source[slice], found, sizeof(found));
            }
        }
        if (source[0][0] && source[1][0] && source[0][1] == source[1][1] && source[0][2] == source[1][2] &&
            source[0][7] == 1 && source[1][7] == 1) {
            bool usable = true;
            for (unsigned v = 0; v < 2; ++v)
                usable = usable && source[v][5] > source[v][3] && source[v][6] > source[v][4] &&
                         source[v][5] <= source[v][1] && source[v][6] <= source[v][2];
            if (usable) {
                gaze.width = source[0][1]; gaze.height = source[0][2];
                for (unsigned v = 0; v < 2; ++v)
                    gaze.rects[v] = {{(int32_t)source[v][3], (int32_t)source[v][4]},
                                     {(int32_t)(source[v][5] - source[v][3]), (int32_t)(source[v][6] - source[v][4])}};
            }
        }
        if (!UNIX_CALL(mw_gaze_foveation, &gaze) && gaze.map != session->gaze_map &&
            SUCCEEDED(session->interop->set_frame_rate_map((void *)gaze.map)))
            session->gaze_map = gaze.map;
    }
    return result;
}
extern "C" XrResult WINAPI xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *info)
{
    if (wine_swapchain_from_handle(swapchain)->session->vulkan) return mw_vk_release(swapchain, info);
    auto *wrapper = wine_swapchain_from_handle(swapchain);
    auto *session = wrapper->session; auto *state = (SessionState *)session->graphics;
    EnterCriticalSection(&state->release_lock);
    auto *swap = (SwapState *)wrapper->graphics;
    if (swap->foveation_failed || (swap->requires_foveation_apply && !swap->foveation_applied)) {
        LeaveCriticalSection(&state->release_lock);
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    HRESULT hr = state->interop->signal_completion(++session->fence_value);
    if (FAILED(hr)) {
        LeaveCriticalSection(&state->release_lock);
        fprintf(stderr, "wineopenxr: ERROR %s producer completion signal failed hr=%#lx\n", state->interop->name(), hr);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    if (swap->foveation_applied) {
        uint32_t index = swap->acquired[swap->acquired_head];
        hr = state->interop->set_target_foveation(swap->images[index].texture, nullptr);
        if (FAILED(hr)) { LeaveCriticalSection(&state->release_lock); return XR_ERROR_RUNTIME_FAILURE; }
        swap->foveation_applied = false;
        swap->foveation_map = 0;
    }
    if (swap->acquired && swap->acquired_count) {
        uint32_t index = swap->acquired[swap->acquired_head];
        if (swap->images[index].copy_source) {
            wrapper->copy_source = swap->images[index].copy_source;
            wrapper->copy_target = swap->metal[index].texture;
        }
    }
    struct xrReleaseSwapchainImage_params params = {swapchain, info};
    NTSTATUS status = UNIX_CALL(xrReleaseSwapchainImage, &params);
    XrResult result = call_result(status, params.result);
    wrapper->copy_source = wrapper->copy_target = nullptr;
    if (XR_SUCCEEDED(result) && swap->acquired_count) {
        swap->acquired_head = (swap->acquired_head + 1) % swap->count;
        --swap->acquired_count;
        swap->waited = false;
    }
    LeaveCriticalSection(&state->release_lock);
    return result;
}
extern "C" XrResult WINAPI xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo *info)
{
    if (wine_swapchain_from_handle(swapchain)->session->vulkan) return mw_vk_wait(swapchain, info);
    struct xrWaitSwapchainImage_params params = {swapchain, info};
    NTSTATUS status = UNIX_CALL(xrWaitSwapchainImage, &params);
    XrResult result = call_result(status, params.result);
    if (result == XR_SUCCESS || result == XR_SESSION_LOSS_PENDING)
        ((SwapState *)wine_swapchain_from_handle(swapchain)->graphics)->waited = true;
    return result;
}
extern "C" XrResult WINAPI xrApplyFoveationD3D11MWXR(XrSwapchain swapchain, uint32_t view,
                                                    XrFoveationD3D11StateMWXR *out)
{
    if (!swapchain || !out || out->type != XR_TYPE_FOVEATION_D3D11_STATE_MWXR || out->next)
        return XR_ERROR_VALIDATION_FAILURE;
    auto *wrapper = wine_swapchain_from_handle(swapchain);
    auto *swap = (SwapState *)wrapper->graphics;
    auto *session = (SessionState *)wrapper->session->graphics;
    if (!wrapper->session->instance->foveation_d3d11_enabled) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (wrapper->info.arraySize != 1 || !session->interop->supports_target_foveation())
        return XR_ERROR_FEATURE_UNSUPPORTED;
    EnterCriticalSection(&session->release_lock);
    if (!swap->images || !swap->waited || swap->acquired_count != 1 || swap->foveation_applied) {
        LeaveCriticalSection(&session->release_lock);
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    mw_foveation_params params = {swapchain, view, 0, out};
    XrResult result = UNIX_CALL(mw_foveation, &params) ? XR_ERROR_RUNTIME_FAILURE : params.result;
    swap->foveation_failed = XR_FAILED(result);
    if (XR_SUCCEEDED(result)) {
        HRESULT hr = session->interop->set_target_foveation(swap->images[swap->acquired[swap->acquired_head]].texture,
                                                           (void *)params.map);
        swap->foveation_failed = FAILED(hr);
        swap->foveation_applied = SUCCEEDED(hr);
        swap->foveation_map = params.map;
        if (FAILED(hr)) result = XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    LeaveCriticalSection(&session->release_lock);
    return result;
}
// Diagnostic only, like MWXRProbeSwapchainImage; not advertised through GIPA.
extern "C" __declspec(dllexport) UINT64 WINAPI MWXRProbeFoveationMap(XrSwapchain swapchain)
{
    return ((SwapState *)wine_swapchain_from_handle(swapchain)->graphics)->foveation_map;
}
extern "C" XrResult WINAPI xrPollEvent(XrInstance instance, XrEventDataBuffer *event)
{
    struct xrPollEvent_params params = {instance, event};
    NTSTATUS status = UNIX_CALL(xrPollEvent, &params);
    XrResult result = call_result(status, params.result);
    if (result == XR_SUCCESS) {
        XrSession *session = nullptr;
        switch (event->type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: session = &((XrEventDataSessionStateChanged *)event)->session; break;
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: session = &((XrEventDataInteractionProfileChanged *)event)->session; break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: session = &((XrEventDataReferenceSpaceChangePending *)event)->session; break;
        case XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR: session = &((XrEventDataVisibilityMaskChangedKHR *)event)->session; break;
        default: break;
        }
        if (session && *session) {
            if (!active_session || *session != active_session->host_session) return XR_ERROR_RUNTIME_FAILURE;
            *session = (XrSession)active_session;
        }
    }
    return result;
}
extern "C" XrResult WINAPI xrEndFrame(XrSession session, const XrFrameEndInfo *info)
{
    if (!info || info->next || info->layerCount > 1) return XR_ERROR_LAYER_INVALID;
    XrFrameEndInfo native = *info;
    XrCompositionLayerProjection projection; XrCompositionLayerProjectionView views[4];
    const XrCompositionLayerBaseHeader *layer = nullptr;
    if (info->layerCount) {
        if (!info->layers || !info->layers[0] || info->layers[0]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            return XR_ERROR_LAYER_INVALID;
        projection = *(const XrCompositionLayerProjection *)info->layers[0];
        // The one chained struct passed on: XR_MNDX_display_distortion's display image marker.
        const auto *marker = (const XrCompositionLayerDisplayImageMNDX *)projection.next;
        if (marker && (marker->type != XR_TYPE_COMPOSITION_LAYER_DISPLAY_IMAGE_MNDX || marker->next))
            return XR_ERROR_LAYER_INVALID;
        // Stereo, or XR_VARJO_quad_views' two context views followed by two insets.
        if ((projection.viewCount != 2 && projection.viewCount != 4) || !projection.views) return XR_ERROR_LAYER_INVALID;
        static uint32_t logged_view_count;
        if (projection.viewCount != logged_view_count) {
            logged_view_count = projection.viewCount;
            fprintf(stderr, "wineopenxr: projection layer with %u views\n", projection.viewCount);
        }
        // Gaze foveation covers one swapchain holding both eyes: side by side in one layer, or
        // one array layer each (xrizer), where the two rectangles overlap and the map is the
        // higher of the two eyes' rates.
        auto *gaze = wine_session_from_handle(session)->vulkan ? nullptr : (SessionState *)wine_session_from_handle(session)->graphics;
        if (gaze && projection.viewCount == 2 && projection.views[0].subImage.swapchain &&
            projection.views[0].subImage.swapchain == projection.views[1].subImage.swapchain) {
            gaze->gaze_swapchain = projection.views[0].subImage.swapchain;
            for (unsigned i = 0; i < 2; ++i) {
                gaze->gaze_rects[i] = projection.views[i].subImage.imageRect; gaze->gaze_fovs[i] = projection.views[i].fov;
            }
        }
        if (wine_session_from_handle(session)->vulkan) mw_vk_note_projection(session, &projection);
        for (unsigned i = 0; i < projection.viewCount; ++i) {
            views[i] = projection.views[i];
            if (views[i].next || !views[i].subImage.swapchain) return XR_ERROR_LAYER_INVALID;
            views[i].subImage.swapchain = wine_swapchain_from_handle(views[i].subImage.swapchain)->host_swapchain;
        }
        projection.views = views; layer = (const XrCompositionLayerBaseHeader *)&projection;
        native.layers = &layer;
    }
    struct xrEndFrame_params params = {session, &native};
    NTSTATUS status = UNIX_CALL(xrEndFrame, &params);
    XrResult result = call_result(status, params.result);
    // After the frame is submitted, as Present marks its boundary.
    if (XR_SUCCEEDED(result) && !wine_session_from_handle(session)->vulkan) {
        auto *state = (SessionState *)wine_session_from_handle(session)->graphics;
        if (state && state->interop) state->interop->end_frame();
    }
    return result;
}
