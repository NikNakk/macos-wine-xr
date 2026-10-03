// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// PE graphics wrappers follow Proton's manual entry-point model. The graphics
// adaptation differs on macOS: a GraphicsInterop backend exposes native
// runtime-owned Metal objects as D3D11 resources. Nothing here is specific to
// the D3D translation layer in use.
#include "openxr_loader.h"
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include "graphics_interop.h"
struct SwapState { wine_XrSwapchain *wrapper; GraphicsImage *images; XrSwapchainImageMetalKHR *metal; uint32_t count; SwapState *next; };
struct SessionState { ID3D11Device *device; GraphicsInterop *interop; SwapState *swapchains;
    CRITICAL_SECTION release_lock; };
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
    wrapper->instance = parent; wrapper->metal_device = state->interop->metal_device(); wrapper->graphics = state;
    struct xrCreateSession_params params = {};
    params.instance = instance; params.createInfo = info; params.session = &wrapper->host_session; params.wine_session = wrapper;
    NTSTATUS status = UNIX_CALL(xrCreateSession, &params);
    XrResult result = call_result(status, params.result);
    if (XR_SUCCEEDED(result)) {
        hr = state->interop->bind_completion_event(wrapper->metal_event);
        if (SUCCEEDED(hr))
            fprintf(stderr, "wineopenxr: session backend=%s api=D3D11 sync=%s\n", state->interop->name(),
                    mw_graphics_sync_name(state->interop->sync()));
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
    if (!info || !swapchain || info->type != XR_TYPE_SWAPCHAIN_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    *swapchain = XR_NULL_HANDLE;
    auto *wrapper = (wine_XrSwapchain *)calloc(1, sizeof(wine_XrSwapchain));
    auto *state = (SwapState *)calloc(1, sizeof(SwapState));
    if (!wrapper || !state) { free(wrapper); free(state); return XR_ERROR_OUT_OF_MEMORY; }
    wrapper->session = wine_session_from_handle(session); wrapper->info = *info; wrapper->graphics = state; state->wrapper = wrapper;
    struct xrCreateSwapchain_params params = {session, info, &wrapper->host_swapchain};
    NTSTATUS status = UNIX_CALL(xrCreateSwapchain, &params);
    XrResult result = call_result(status, params.result);
    if (XR_FAILED(result)) { free(state); free(wrapper); return result; }
    auto *parent = (SessionState *)wrapper->session->graphics;
    state->next = parent->swapchains; parent->swapchains = state;
    *swapchain = (XrSwapchain)wrapper;
    return result;
}
extern "C" XrResult WINAPI xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t capacity,
    uint32_t *count, XrSwapchainImageBaseHeader *images)
{
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
        if (!metal || !images) { free(metal); free(images); return XR_ERROR_OUT_OF_MEMORY; }
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
            free(metal); free(images); return result;
        }
        // One summary per swapchain; release paths never log per frame.
        fprintf(stderr, "wineopenxr: swapchain=%p backend=%s api=D3D11 images=%u %ux%u array=%u format=%u "
                "native=MTLTexture iosurface=%u/%u sync=%s zero-copy=%s%s\n", (void *)wrapper, interop->name(),
                state->count, desc.Width, desc.Height, desc.ArraySize, (unsigned)desc.Format, iosurface, state->count,
                mw_graphics_sync_name(interop->sync()), zero_copy == state->count ? "yes" : "no",
                zero_copy == state->count ? "" : " fallback=explicit-gpu-blit");
        state->images = images; state->metal = metal;
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
    auto *state = (SwapState *)wrapper->graphics;
    if (!state->metal || index >= state->count) return XR_ERROR_INDEX_OUT_OF_RANGE;
    *device = (UINT64)wrapper->session->metal_device;
    *texture = (UINT64)state->metal[index].texture;
    return XR_SUCCESS;
}
extern "C" XrResult WINAPI xrDestroySwapchain(XrSwapchain swapchain)
{
    wine_XrSwapchain *wrapper = wine_swapchain_from_handle(swapchain);
    auto *state = (SwapState *)wrapper->graphics;
    struct xrDestroySwapchain_params params = {swapchain};
    NTSTATUS status = UNIX_CALL(xrDestroySwapchain, &params);
    XrResult result = call_result(status, params.result);
    if (XR_FAILED(result)) return result;
    auto *parent = (SessionState *)wrapper->session->graphics;
    SwapState **link = &parent->swapchains;
    while (*link && *link != state) link = &(*link)->next;
    if (*link) *link = state->next;
    for (uint32_t i = 0; state->images && i < state->count; ++i) parent->interop->release_image(&state->images[i]);
    free(state->metal); free(state->images); free(state); free(wrapper);
    return result;
}
extern "C" XrResult WINAPI xrDestroySession(XrSession session)
{
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
extern "C" XrResult WINAPI xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *info)
{
    auto *wrapper = wine_swapchain_from_handle(swapchain);
    auto *session = wrapper->session; auto *state = (SessionState *)session->graphics;
    EnterCriticalSection(&state->release_lock);
    HRESULT hr = state->interop->signal_completion(++session->fence_value);
    if (FAILED(hr)) {
        LeaveCriticalSection(&state->release_lock);
        fprintf(stderr, "wineopenxr: ERROR %s producer completion signal failed hr=%#lx\n", state->interop->name(), hr);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    struct xrReleaseSwapchainImage_params params = {swapchain, info};
    NTSTATUS status = UNIX_CALL(xrReleaseSwapchainImage, &params);
    XrResult result = call_result(status, params.result);
    LeaveCriticalSection(&state->release_lock);
    return result;
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
    XrCompositionLayerProjection projection; XrCompositionLayerProjectionView views[2];
    const XrCompositionLayerBaseHeader *layer = nullptr;
    if (info->layerCount) {
        if (!info->layers || !info->layers[0] || info->layers[0]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            return XR_ERROR_LAYER_INVALID;
        projection = *(const XrCompositionLayerProjection *)info->layers[0];
        if (projection.next || projection.viewCount != 2 || !projection.views) return XR_ERROR_LAYER_INVALID;
        for (unsigned i = 0; i < 2; ++i) {
            views[i] = projection.views[i];
            if (views[i].next || !views[i].subImage.swapchain) return XR_ERROR_LAYER_INVALID;
            views[i].subImage.swapchain = wine_swapchain_from_handle(views[i].subImage.swapchain)->host_swapchain;
        }
        projection.views = views; layer = (const XrCompositionLayerBaseHeader *)&projection;
        native.layers = &layer;
    }
    struct xrEndFrame_params params = {session, &native};
    NTSTATUS status = UNIX_CALL(xrEndFrame, &params);
    return call_result(status, params.result);
}
