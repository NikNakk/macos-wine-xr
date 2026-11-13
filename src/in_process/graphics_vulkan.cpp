// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// All Vulkan calls use Wine's PE Vulkan API. Only opaque Metal objects cross
// the unix boundary: Wine's dispatchable Vulkan handles must never do so.
#include "graphics_vulkan.h"
#include <vulkan/vulkan_metal.h>
#include <stdio.h>

static INIT_ONCE loader_once = INIT_ONCE_STATIC_INIT;
static PFN_vkGetInstanceProcAddr gipa;
static BOOL CALLBACK load_vulkan(PINIT_ONCE, PVOID, PVOID *)
{
    HMODULE module = LoadLibraryA("vulkan-1.dll");
    if (module) gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(module, "vkGetInstanceProcAddr");
    // Keep the loader loaded for the lifetime of application Vulkan objects.
    return TRUE;
}
static bool load_vulkan()
{
    InitOnceExecuteOnce(&loader_once, load_vulkan, nullptr, nullptr);
    return gipa != nullptr;
}
template<typename T> static T instance_proc(VkInstance instance, const char *name)
{ return (T)gipa(instance, name); }
static XrResult vk_result(VkResult r)
{
    if (r == VK_SUCCESS) return XR_SUCCESS;
    if (r == VK_ERROR_OUT_OF_HOST_MEMORY || r == VK_ERROR_OUT_OF_DEVICE_MEMORY) return XR_ERROR_OUT_OF_MEMORY;
    if (r == VK_ERROR_DEVICE_LOST) return XR_ERROR_SESSION_LOST;
    return XR_ERROR_GRAPHICS_DEVICE_INVALID;
}
static XrResult native_call(wine_XrInstance *instance, XrSystemId system, uint32_t op,
                            uint64_t object = 0, uint32_t *array_texture = nullptr)
{
    mw_vulkan_native_params p = {instance, system, object, op, 0, XR_ERROR_RUNTIME_FAILURE};
    if (UNIX_CALL(mw_vulkan_native, &p)) return XR_ERROR_RUNTIME_FAILURE;
    if (array_texture) *array_texture = p.array_texture;
    return p.result;
}
static bool enabled(XrInstance instance, bool v2)
{
    return instance && (v2 ? wine_instance_from_handle(instance)->vulkan2_enabled :
                            wine_instance_from_handle(instance)->vulkan_enabled);
}
static XrResult requirements(XrInstance instance, XrSystemId system, XrGraphicsRequirementsVulkanKHR *out, bool v2)
{
    if (!enabled(instance, v2)) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!out || out->type != XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR || out->next) return XR_ERROR_VALIDATION_FAILURE;
    XrResult r = native_call(wine_instance_from_handle(instance), system, MW_VK_REQUIREMENTS);
    if (XR_SUCCEEDED(r)) {
        out->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
        out->maxApiVersionSupported = XR_MAKE_VERSION(1, 4, 0);
        wine_instance_from_handle(instance)->vulkan_requirements_called = 1;
    }
    return r;
}
extern "C" XrResult WINAPI xrGetVulkanGraphicsRequirementsKHR(XrInstance i, XrSystemId s, XrGraphicsRequirementsVulkanKHR *r)
{ return requirements(i, s, r, false); }
extern "C" XrResult WINAPI xrGetVulkanGraphicsRequirements2KHR(XrInstance i, XrSystemId s, XrGraphicsRequirementsVulkanKHR *r)
{ return requirements(i, s, r, true); }

static XrResult extension_string(XrInstance instance, XrSystemId system, uint32_t capacity, uint32_t *count,
                                  char *buffer, const char *extensions)
{
    if (!enabled(instance, false)) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!count || (capacity && !buffer)) return XR_ERROR_VALIDATION_FAILURE;
    // Validate the system even when the application only asks for the size.
    XrResult r = native_call(wine_instance_from_handle(instance), system, MW_VK_REQUIREMENTS);
    if (XR_FAILED(r)) return r;
    *count = (uint32_t)strlen(extensions) + 1;
    if (!capacity) return XR_SUCCESS;
    if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    fprintf(stderr, "wineopenxr: Vulkan required extensions: %s\n", extensions);
    memcpy(buffer, extensions, *count);
    return XR_SUCCESS;
}
extern "C" XrResult WINAPI xrGetVulkanInstanceExtensionsKHR(XrInstance i, XrSystemId s, uint32_t n, uint32_t *c, char *b)
{ return extension_string(i, s, n, c, b, ""); }
extern "C" XrResult WINAPI xrGetVulkanDeviceExtensionsKHR(XrInstance i, XrSystemId s, uint32_t n, uint32_t *c, char *b)
{ return extension_string(i, s, n, c, b, VK_EXT_METAL_OBJECTS_EXTENSION_NAME); }

// Export a device's actual Metal identity; selecting Vulkan device zero would
// silently choose the wrong GPU on machines with more than one adapter.
static XrResult match_device(wine_XrInstance *parent, XrSystemId system, VkInstance instance,
                             VkDevice device, uint64_t *metal = nullptr)
{
    auto gdpa = instance_proc<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
    auto export_objects = gdpa ? (PFN_vkExportMetalObjectsEXT)gdpa(device, "vkExportMetalObjectsEXT") : nullptr;
    if (!export_objects) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    VkExportMetalDeviceInfoEXT identity = {VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
    VkExportMetalObjectsInfoEXT objects = {VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &identity};
    export_objects(device, &objects);
    if (!identity.mtlDevice) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    XrResult r = native_call(parent, system, MW_VK_MATCH_DEVICE, (uint64_t)identity.mtlDevice);
    if (XR_SUCCEEDED(r) && metal) *metal = (uint64_t)identity.mtlDevice;
    return r;
}
static XrResult graphics_device(XrInstance xr, XrSystemId system, VkInstance instance, VkPhysicalDevice *out)
{
    if (!instance || !out) return XR_ERROR_VALIDATION_FAILURE;
    *out = VK_NULL_HANDLE;
    if (!load_vulkan()) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    auto *parent = wine_instance_from_handle(xr);
    XrResult r = native_call(parent, system, MW_VK_REQUIREMENTS);
    if (XR_FAILED(r)) return r;
    auto enumerate = instance_proc<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
    auto queues = instance_proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    auto extensions = instance_proc<PFN_vkEnumerateDeviceExtensionProperties>(instance, "vkEnumerateDeviceExtensionProperties");
    auto create = instance_proc<PFN_vkCreateDevice>(instance, "vkCreateDevice");
    auto gdpa = instance_proc<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
    if (!enumerate || !queues || !extensions || !create || !gdpa) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    uint32_t count = 0;
    VkResult vr = enumerate(instance, &count, nullptr);
    if (vr != VK_SUCCESS || !count) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    auto *devices = (VkPhysicalDevice *)calloc(count, sizeof(VkPhysicalDevice));
    if (!devices) return XR_ERROR_OUT_OF_MEMORY;
    vr = enumerate(instance, &count, devices);
    r = XR_ERROR_GRAPHICS_DEVICE_INVALID;
    for (uint32_t i = 0; vr == VK_SUCCESS && i < count; ++i) {
        uint32_t ec = 0, qc = 0;
        if (extensions(devices[i], nullptr, &ec, nullptr) != VK_SUCCESS) continue;
        auto *ext = (VkExtensionProperties *)calloc(ec, sizeof(VkExtensionProperties));
        if (!ext) { r = XR_ERROR_OUT_OF_MEMORY; break; }
        bool has_metal = false;
        if (extensions(devices[i], nullptr, &ec, ext) == VK_SUCCESS)
            for (uint32_t j = 0; j < ec; ++j) has_metal |= !strcmp(ext[j].extensionName, VK_EXT_METAL_OBJECTS_EXTENSION_NAME);
        free(ext);
        if (!has_metal) continue;
        queues(devices[i], &qc, nullptr);
        auto *families = (VkQueueFamilyProperties *)calloc(qc, sizeof(VkQueueFamilyProperties));
        if (!families) { r = XR_ERROR_OUT_OF_MEMORY; break; }
        queues(devices[i], &qc, families);
        uint32_t family = 0;
        while (family < qc && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
        free(families);
        if (family == qc) continue;
        float priority = 1;
        VkDeviceQueueCreateInfo queue = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, family, 1, &priority};
        const char *extension = VK_EXT_METAL_OBJECTS_EXTENSION_NAME;
        VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        ci.queueCreateInfoCount = 1; ci.pQueueCreateInfos = &queue;
        ci.enabledExtensionCount = 1; ci.ppEnabledExtensionNames = &extension;
        VkDevice probe = VK_NULL_HANDLE;
        if (create(devices[i], &ci, nullptr, &probe) != VK_SUCCESS) continue;
        XrResult match = match_device(parent, system, instance, probe);
        ((PFN_vkDestroyDevice)gdpa(probe, "vkDestroyDevice"))(probe, nullptr);
        if (XR_SUCCEEDED(match)) { *out = devices[i]; parent->vulkan_instance = instance; parent->vulkan_physical = devices[i]; r = XR_SUCCESS; break; }
    }
    free(devices);
    return r;
}
extern "C" XrResult WINAPI xrGetVulkanGraphicsDeviceKHR(XrInstance i, XrSystemId s, VkInstance v, VkPhysicalDevice *p)
{
    if (!enabled(i, false)) return XR_ERROR_FUNCTION_UNSUPPORTED;
    return graphics_device(i, s, v, p);
}
extern "C" XrResult WINAPI xrGetVulkanGraphicsDevice2KHR(XrInstance i, const XrVulkanGraphicsDeviceGetInfoKHR *info, VkPhysicalDevice *p)
{
    if (!enabled(i, true)) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!info || info->type != XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR || info->next) return XR_ERROR_VALIDATION_FAILURE;
    return graphics_device(i, info->systemId, info->vulkanInstance, p);
}
extern "C" XrResult WINAPI xrCreateVulkanInstanceKHR(XrInstance i, const XrVulkanInstanceCreateInfoKHR *info,
                                                     VkInstance *instance, VkResult *result)
{
    if (!enabled(i, true)) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!info || info->type != XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR || info->next || info->createFlags ||
        !info->pfnGetInstanceProcAddr || !info->vulkanCreateInfo || !instance || !result) return XR_ERROR_VALIDATION_FAILURE;
    XrResult r = native_call(wine_instance_from_handle(i), info->systemId, MW_VK_REQUIREMENTS);
    if (XR_FAILED(r)) return r;
    auto create = (PFN_vkCreateInstance)info->pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    *result = create(info->vulkanCreateInfo, info->vulkanAllocator, instance);
    if (*result == VK_SUCCESS) wine_instance_from_handle(i)->vulkan_instance = *instance;
    return XR_SUCCESS;
}
extern "C" XrResult WINAPI xrCreateVulkanDeviceKHR(XrInstance i, const XrVulkanDeviceCreateInfoKHR *info,
                                                   VkDevice *device, VkResult *result)
{
    if (!enabled(i, true)) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!info || info->type != XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR || info->next || info->createFlags ||
        !info->pfnGetInstanceProcAddr || !info->vulkanCreateInfo || !device || !result) return XR_ERROR_VALIDATION_FAILURE;
    auto *parent = wine_instance_from_handle(i);
    if (!parent->vulkan_instance || parent->vulkan_physical != info->vulkanPhysicalDevice)
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    XrResult query = native_call(parent, info->systemId, MW_VK_REQUIREMENTS);
    if (XR_FAILED(query)) return query;
    VkDeviceCreateInfo ci = *info->vulkanCreateInfo;
    auto **ext = (const char **)calloc(ci.enabledExtensionCount + 1, sizeof(const char *));
    if (!ext) return XR_ERROR_OUT_OF_MEMORY;
    bool has_metal = false;
    for (uint32_t j = 0; j < ci.enabledExtensionCount; ++j) {
        ext[j] = ci.ppEnabledExtensionNames[j];
        has_metal |= !strcmp(ext[j], VK_EXT_METAL_OBJECTS_EXTENSION_NAME);
    }
    if (!has_metal) ext[ci.enabledExtensionCount++] = VK_EXT_METAL_OBJECTS_EXTENSION_NAME;
    ci.ppEnabledExtensionNames = ext;
    auto create = (PFN_vkCreateDevice)info->pfnGetInstanceProcAddr(parent->vulkan_instance, "vkCreateDevice");
    if (!create) { free(ext); return XR_ERROR_GRAPHICS_DEVICE_INVALID; }
    *result = create(info->vulkanPhysicalDevice, &ci, info->vulkanAllocator, device);
    free(ext);
    return XR_SUCCESS;
}

struct VulkanSwap;
struct VulkanSession {
    VkInstance instance; VkPhysicalDevice physical; VkDevice device; VkQueue queue;
    PFN_vkGetDeviceProcAddr gdpa;
    VkFence fence;
    // GPU ordering: a timeline semaphore on the session's MTLSharedEvent. Release signals
    // the next value after the application's queue work; Metal waits for it on the GPU.
    VkSemaphore timeline;
    CRITICAL_SECTION lock;
    VulkanSwap *swaps;
    // Gaze foveation: the swapchain holding both eyes, as last submitted.
    XrSwapchain gaze_swapchain; XrRect2Di gaze_rects[2]; XrFovf gaze_fovs[2];
};
struct VulkanSwap {
    wine_XrSwapchain *wrapper;
    VkImage *images;
    uint32_t count, acquired, waited;
    VulkanSwap *next;
    void **textures; uint32_t order[16], head, queued; // native textures; acquired indices in FIFO order
};
template<typename T> static T device_proc(VulkanSession *s, const char *name)
{ return (T)s->gdpa(s->device, name); }
// Release: signal the session's shared event after everything already submitted to the
// application's queue (the eye rendering or xrizer's copy). The native release encodes a
// GPU wait for that value before the runtime's own work, as the D3D11 path does.
static XrResult signal_release(VulkanSession *s, wine_XrSession *session)
{
    uint64_t value = session->fence_value + 1;
    VkTimelineSemaphoreSubmitInfo values = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    values.signalSemaphoreValueCount = 1; values.pSignalSemaphoreValues = &value;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &values};
    submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &s->timeline;
    VkResult r = device_proc<PFN_vkQueueSubmit>(s, "vkQueueSubmit")(s->queue, 1, &submit, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) session->fence_value = value;
    return vk_result(r);
}
static XrResult complete(VulkanSession *s)
{
    // A queue fence orders the submitted eye copy without requiring timeline
    // semaphore features that legacy OpenVR applications did not enable.
    VkResult r = device_proc<PFN_vkResetFences>(s, "vkResetFences")(s->device, 1, &s->fence);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (r == VK_SUCCESS) r = device_proc<PFN_vkQueueSubmit>(s, "vkQueueSubmit")(s->queue, 1, &submit, s->fence);
    if (r == VK_SUCCESS) r = device_proc<PFN_vkWaitForFences>(s, "vkWaitForFences")(s->device, 1, &s->fence, VK_TRUE, UINT64_MAX);
    return vk_result(r);
}
XrResult mw_vk_create_session(XrInstance xr, const XrSessionCreateInfo *info, XrSession *out)
{
    auto *parent = wine_instance_from_handle(xr);
    if (!parent->vulkan_enabled && !parent->vulkan2_enabled) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    if (!parent->vulkan_requirements_called) return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
    const auto *binding = (const XrGraphicsBindingVulkanKHR *)info->next;
    if (!binding || binding->type != XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR || binding->next ||
        !binding->instance || !binding->physicalDevice || !binding->device) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    if (!load_vulkan()) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    VkPhysicalDevice expected = VK_NULL_HANDLE;
    XrResult r = graphics_device(xr, info->systemId, binding->instance, &expected);
    if (XR_FAILED(r) || expected != binding->physicalDevice) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    auto *s = (VulkanSession *)calloc(1, sizeof(VulkanSession));
    auto *wrapper = (wine_XrSession *)calloc(1, sizeof(wine_XrSession));
    if (!s || !wrapper) { free(s); free(wrapper); return XR_ERROR_OUT_OF_MEMORY; }
    s->instance = binding->instance; s->physical = binding->physicalDevice; s->device = binding->device;
    s->gdpa = instance_proc<PFN_vkGetDeviceProcAddr>(s->instance, "vkGetDeviceProcAddr");
    uint64_t metal = 0;
    r = match_device(parent, info->systemId, s->instance, s->device, &metal);
    auto queues = instance_proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(s->instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t count = 0; queues(s->physical, &count, nullptr);
    auto *families = (VkQueueFamilyProperties *)calloc(count, sizeof(VkQueueFamilyProperties));
    if (!families) r = XR_ERROR_OUT_OF_MEMORY;
    if (families) {
        queues(s->physical, &count, families);
        if (binding->queueFamilyIndex >= count || binding->queueIndex >= families[binding->queueFamilyIndex].queueCount ||
            !(families[binding->queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT)) r = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        free(families);
    }
    if (XR_FAILED(r)) { free(s); free(wrapper); return r; }
    device_proc<PFN_vkGetDeviceQueue>(s, "vkGetDeviceQueue")(s->device, binding->queueFamilyIndex, binding->queueIndex, &s->queue);
    if (!s->queue) { free(s); free(wrapper); return XR_ERROR_GRAPHICS_DEVICE_INVALID; }
    VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    r = vk_result(device_proc<PFN_vkCreateFence>(s, "vkCreateFence")(s->device, &fence, nullptr, &s->fence));
    if (XR_FAILED(r)) { free(s); free(wrapper); return r; }
    InitializeCriticalSection(&s->lock);
    wrapper->instance = parent; wrapper->metal_device = (void *)metal; wrapper->graphics = s; wrapper->vulkan = 1;
    xrCreateSession_params p = {};
    p.instance = xr; p.createInfo = info; p.session = &wrapper->host_session; p.wine_session = wrapper;
    r = UNIX_CALL(xrCreateSession, &p) ? XR_ERROR_RUNTIME_FAILURE : p.result;
    if (XR_FAILED(r)) {
        device_proc<PFN_vkDestroyFence>(s, "vkDestroyFence")(s->device, s->fence, nullptr);
        DeleteCriticalSection(&s->lock); free(s); free(wrapper); return r;
    }
    // The native session made its MTLSharedEvent; give it to Vulkan as a timeline semaphore
    // (VK_EXT_metal_objects). MWXR_VULKAN_SYNC=fence keeps the CPU fence wait.
    const char *sync = getenv("MWXR_VULKAN_SYNC");
    if (wrapper->metal_event && !(sync && !strcmp(sync, "fence"))) {
        VkImportMetalSharedEventInfoEXT import = {VK_STRUCTURE_TYPE_IMPORT_METAL_SHARED_EVENT_INFO_EXT};
        import.mtlSharedEvent = (MTLSharedEvent_id)wrapper->metal_event;
        VkSemaphoreTypeCreateInfo type = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, &import};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE; type.initialValue = wrapper->fence_value;
        VkSemaphoreCreateInfo create = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        VkResult vr = device_proc<PFN_vkCreateSemaphore>(s, "vkCreateSemaphore")(s->device, &create, nullptr, &s->timeline);
        if (vr != VK_SUCCESS) {
            s->timeline = VK_NULL_HANDLE;
            fprintf(stderr, "wineopenxr: Vulkan timeline on the Metal event failed (%d); using the CPU fence wait\n", vr);
        }
    }
    fprintf(stderr, "wineopenxr: session backend=kosmickrisp api=Vulkan sync=%s native=Metal\n",
            s->timeline ? "gpu-shared-event" : "fence-cpu-wait");
    *out = (XrSession)wrapper;
    return r;
}
XrResult mw_vk_create_swapchain(XrSession session, const XrSwapchainCreateInfo *info, XrSwapchain *out)
{
    if (!info || !out || info->type != XR_TYPE_SWAPCHAIN_CREATE_INFO || info->next) return XR_ERROR_VALIDATION_FAILURE;
    *out = XR_NULL_HANDLE;
    auto *wrapper = (wine_XrSwapchain *)calloc(1, sizeof(wine_XrSwapchain));
    auto *swap = (VulkanSwap *)calloc(1, sizeof(VulkanSwap));
    if (!wrapper || !swap) { free(wrapper); free(swap); return XR_ERROR_OUT_OF_MEMORY; }
    wrapper->session = wine_session_from_handle(session); wrapper->info = *info; wrapper->graphics = swap; swap->wrapper = wrapper;
    xrCreateSwapchain_params p = {session, info, &wrapper->host_swapchain};
    XrResult r = UNIX_CALL(xrCreateSwapchain, &p) ? XR_ERROR_RUNTIME_FAILURE : p.result;
    if (XR_FAILED(r)) { free(wrapper); free(swap); return r; }
    auto *s = (VulkanSession *)wrapper->session->graphics;
    swap->next = s->swaps; s->swaps = swap;
    *out = (XrSwapchain)wrapper;
    return r;
}
static VkImageUsageFlags image_usage(XrSwapchainUsageFlags u)
{
    VkImageUsageFlags v = 0;
    if (u & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) v |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (u & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) v |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (u & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) v |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (u & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) v |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return v;
}
XrResult mw_vk_enumerate_images(XrSwapchain sc, uint32_t capacity, uint32_t *count, XrSwapchainImageBaseHeader *out)
{
    if (!count || (capacity && !out)) return XR_ERROR_VALIDATION_FAILURE;
    auto *wrapper = wine_swapchain_from_handle(sc);
    auto *swap = (VulkanSwap *)wrapper->graphics;
    auto *s = (VulkanSession *)wrapper->session->graphics;
    xrEnumerateSwapchainImages_params p = {sc, 0, &swap->count, nullptr};
    XrResult r = UNIX_CALL(xrEnumerateSwapchainImages, &p) ? XR_ERROR_RUNTIME_FAILURE : p.result;
    if (XR_FAILED(r)) return r;
    *count = swap->count;
    if (!capacity) return XR_SUCCESS;
    if (capacity < swap->count) return XR_ERROR_SIZE_INSUFFICIENT;
    auto *images = (XrSwapchainImageVulkanKHR *)out;
    for (uint32_t i = 0; i < swap->count; ++i)
        if (images[i].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR || images[i].next) return XR_ERROR_VALIDATION_FAILURE;
    if (!swap->images) {
        auto *metal = (XrSwapchainImageMetalKHR *)calloc(swap->count, sizeof(XrSwapchainImageMetalKHR));
        auto *vk = (VkImage *)calloc(swap->count, sizeof(VkImage));
        if (!metal || !vk) { free(metal); free(vk); return XR_ERROR_OUT_OF_MEMORY; }
        for (uint32_t i = 0; i < swap->count; ++i) metal[i].type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
        p.imageCapacityInput = swap->count; p.images = (XrSwapchainImageBaseHeader *)metal;
        r = UNIX_CALL(xrEnumerateSwapchainImages, &p) ? XR_ERROR_RUNTIME_FAILURE : p.result;
        for (uint32_t i = 0; XR_SUCCEEDED(r) && i < swap->count; ++i) {
            uint32_t is_array = 0;
            r = native_call(wrapper->session->instance, 0, MW_VK_TEXTURE_TYPE, (uint64_t)metal[i].texture, &is_array);
            if (XR_FAILED(r)) break;
            VkImportMetalTextureInfoEXT import = {VK_STRUCTURE_TYPE_IMPORT_METAL_TEXTURE_INFO_EXT};
            import.mtlTexture = (MTLTexture_id)metal[i].texture;
            VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &import};
            ci.imageType = VK_IMAGE_TYPE_2D; ci.format = (VkFormat)wrapper->info.format;
            ci.extent = {wrapper->info.width, wrapper->info.height, 1};
            ci.mipLevels = 1; ci.arrayLayers = wrapper->info.arraySize; ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = image_usage(wrapper->info.usageFlags);
            // KK uses an array texture for input attachments, including one-layer arrays.
            if (is_array && ci.arrayLayers == 1) ci.usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
            if (wrapper->info.usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            r = vk_result(device_proc<PFN_vkCreateImage>(s, "vkCreateImage")(s->device, &ci, nullptr, &vk[i]));
        }
        if (XR_SUCCEEDED(r) && (swap->textures = (void **)calloc(swap->count, sizeof(void *))))
            for (uint32_t i = 0; i < swap->count; ++i) swap->textures[i] = metal[i].texture;
        free(metal);
        if (XR_FAILED(r)) {
            for (uint32_t i = 0; i < swap->count; ++i)
                if (vk[i]) device_proc<PFN_vkDestroyImage>(s, "vkDestroyImage")(s->device, vk[i], nullptr);
            free(vk); return r;
        }
        swap->images = vk;
        fprintf(stderr, "wineopenxr: swapchain=%p api=Vulkan images=%u %ux%u array=%u format=%lld zero-copy=yes sync=fence-cpu-wait\n",
                wrapper, swap->count, wrapper->info.width, wrapper->info.height, wrapper->info.arraySize, (long long)wrapper->info.format);
    }
    for (uint32_t i = 0; i < swap->count; ++i) images[i].image = swap->images[i];
    return XR_SUCCESS;
}
XrResult mw_vk_acquire(XrSwapchain sc, const XrSwapchainImageAcquireInfo *info, uint32_t *index)
{
    auto *swap = (VulkanSwap *)wine_swapchain_from_handle(sc)->graphics;
    xrAcquireSwapchainImage_params p = {sc, info, index};
    XrResult r = UNIX_CALL(xrAcquireSwapchainImage, &p) ? XR_ERROR_RUNTIME_FAILURE : p.result;
    if (XR_SUCCEEDED(r)) ++swap->acquired;
    if (XR_SUCCEEDED(r) && swap->queued < 16) swap->order[(swap->head + swap->queued++) % 16] = *index;
    // Once per frame, before the application renders it: the gaze-centred profile, which
    // the bridge hands to KosmicKrisp (MWXR_KK_FOVEATION). The runtime images stay ordinary.
    auto *wrapper = wine_swapchain_from_handle(sc);
    auto *s = (VulkanSession *)wrapper->session->graphics;
    if (XR_SUCCEEDED(r) && wrapper->session->instance->gaze_foveation && sc == s->gaze_swapchain) {
        mw_gaze_foveation_params gaze = {sc, wrapper->info.width, wrapper->info.height,
            {s->gaze_rects[0], s->gaze_rects[1]}, {s->gaze_fovs[0], s->gaze_fovs[1]}, 0, XR_SUCCESS};
        UNIX_CALL(mw_gaze_foveation, &gaze);
    }
    return r;
}
XrResult mw_vk_wait(XrSwapchain sc, const XrSwapchainImageWaitInfo *info)
{
    auto *swap = (VulkanSwap *)wine_swapchain_from_handle(sc)->graphics;
    xrWaitSwapchainImage_params p = {sc, info};
    XrResult r = UNIX_CALL(xrWaitSwapchainImage, &p) ? XR_ERROR_RUNTIME_FAILURE : p.result;
    if (r == XR_SUCCESS || r == XR_SESSION_LOSS_PENDING) ++swap->waited;
    return r;
}
XrResult mw_vk_release(XrSwapchain sc, const XrSwapchainImageReleaseInfo *info)
{
    auto *wrapper = wine_swapchain_from_handle(sc);
    auto *swap = (VulkanSwap *)wrapper->graphics;
    auto *s = (VulkanSession *)wrapper->session->graphics;
    if (!swap->acquired || !swap->waited) return XR_ERROR_CALL_ORDER_INVALID;
    EnterCriticalSection(&s->lock);
    XrResult r = s->timeline ? signal_release(s, wrapper->session) : complete(s);
    if (XR_SUCCEEDED(r)) {
        xrReleaseSwapchainImage_params p = {sc, info};
        uint32_t index = swap->queued ? swap->order[swap->head] : UINT32_MAX;
        wrapper->capture_texture = swap->textures && index < swap->count ? swap->textures[index] : nullptr;
        r = UNIX_CALL(xrReleaseSwapchainImage, &p) ? XR_ERROR_RUNTIME_FAILURE : p.result;
        wrapper->capture_texture = nullptr;
        if (XR_SUCCEEDED(r)) { --swap->acquired; --swap->waited; }
        if (XR_SUCCEEDED(r) && swap->queued) { swap->head = (swap->head + 1) % 16; --swap->queued; }
    }
    LeaveCriticalSection(&s->lock);
    return r;
}
XrResult mw_vk_destroy_swapchain(XrSwapchain sc)
{
    auto *wrapper = wine_swapchain_from_handle(sc);
    auto *swap = (VulkanSwap *)wrapper->graphics;
    auto *s = (VulkanSession *)wrapper->session->graphics;
    XrResult r = complete(s);
    if (XR_FAILED(r)) return r;
    xrDestroySwapchain_params p = {sc};
    r = UNIX_CALL(xrDestroySwapchain, &p) ? XR_ERROR_RUNTIME_FAILURE : p.result;
    if (XR_FAILED(r)) return r;
    // Imported images retain their Metal storage independently of the runtime.
    for (uint32_t i = 0; swap->images && i < swap->count; ++i)
        device_proc<PFN_vkDestroyImage>(s, "vkDestroyImage")(s->device, swap->images[i], nullptr);
    free(swap->textures);
    if (s->gaze_swapchain == sc) s->gaze_swapchain = XR_NULL_HANDLE;
    VulkanSwap **link = &s->swaps;
    while (*link && *link != swap) link = &(*link)->next;
    if (*link) *link = swap->next;
    free(swap->images); free(swap); free(wrapper);
    return r;
}
XrResult mw_vk_destroy_session(XrSession session)
{
    auto *wrapper = wine_session_from_handle(session);
    auto *s = (VulkanSession *)wrapper->graphics;
    while (s->swaps) {
        XrResult r = mw_vk_destroy_swapchain((XrSwapchain)s->swaps->wrapper);
        if (XR_FAILED(r)) return r;
    }
    XrResult r = complete(s);
    if (XR_FAILED(r)) return r;
    // Before the native session releases the shared event the semaphore holds.
    if (s->timeline) device_proc<PFN_vkDestroySemaphore>(s, "vkDestroySemaphore")(s->device, s->timeline, nullptr);
    s->timeline = VK_NULL_HANDLE;
    xrDestroySession_params p = {session};
    r = UNIX_CALL(xrDestroySession, &p) ? XR_ERROR_RUNTIME_FAILURE : p.result;
    if (XR_FAILED(r)) return r;
    device_proc<PFN_vkDestroyFence>(s, "vkDestroyFence")(s->device, s->fence, nullptr);
    DeleteCriticalSection(&s->lock); free(s); free(wrapper);
    return r;
}

void mw_vk_note_projection(XrSession session, const XrCompositionLayerProjection *projection)
{
    auto *s = (VulkanSession *)wine_session_from_handle(session)->graphics;
    // One swapchain holding both eyes: side by side, or one array layer each (xrizer).
    if (!s || projection->viewCount != 2 || !projection->views[0].subImage.swapchain ||
        projection->views[0].subImage.swapchain != projection->views[1].subImage.swapchain) return;
    s->gaze_swapchain = projection->views[0].subImage.swapchain;
    for (unsigned i = 0; i < 2; ++i) {
        s->gaze_rects[i] = projection->views[i].subImage.imageRect; s->gaze_fovs[i] = projection->views[i].fov;
    }
}
