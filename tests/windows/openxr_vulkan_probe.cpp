// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#define XR_USE_GRAPHICS_API_VULKAN
#include <windows.h>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#define CHECK(x) do { auto check_result = (x); if (check_result != 0) { fprintf(stderr, "%s: %d line %d\n", #x, (int)check_result, __LINE__); return 1; } } while (0)
#define XR_PROC(name) PFN_##name name = nullptr; CHECK(xrGetInstanceProcAddr(instance, #name, (PFN_xrVoidFunction *)&name))
#define VK_PROC(name) auto name = (PFN_##name)vgipa(vk_instance, #name); if (!name) return 2

int main(int argc, char **argv)
{
    bool v2 = argc > 1 && !strcmp(argv[1], "--vulkan2");
    HMODULE xr = LoadLibraryA("wineopenxr.dll"), vk = LoadLibraryA("vulkan-1.dll");
    if (!xr || !vk) { fprintf(stderr, "load failed %lu\n", GetLastError()); return 2; }
    auto xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)GetProcAddress(xr, "xrGetInstanceProcAddr");
    auto vgipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
    if (!xrGetInstanceProcAddr || !vgipa) return 2;
    XrInstance instance = XR_NULL_HANDLE;
    XR_PROC(xrCreateInstance);
    XR_PROC(xrEnumerateInstanceExtensionProperties);
    uint32_t count = 0;
    CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
    std::vector<XrExtensionProperties> ext(count, {XR_TYPE_EXTENSION_PROPERTIES});
    CHECK(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, ext.data()));
    const char *extension = v2 ? XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME : XR_KHR_VULKAN_ENABLE_EXTENSION_NAME;
    bool found = false;
    for (const auto &e : ext) found |= !strcmp(e.extensionName, extension);
    if (!found) return 3;
    XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ici.applicationInfo.applicationName, "Vulkan Metal bridge probe");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = 1; ici.enabledExtensionNames = &extension;
    CHECK(xrCreateInstance(&ici, &instance));
    XR_PROC(xrGetSystem); XR_PROC(xrCreateSession); XR_PROC(xrDestroySession);
    XR_PROC(xrDestroyInstance); XR_PROC(xrCreateSwapchain); XR_PROC(xrDestroySwapchain);
    XR_PROC(xrEnumerateSwapchainFormats); XR_PROC(xrEnumerateSwapchainImages);
    XR_PROC(xrAcquireSwapchainImage); XR_PROC(xrWaitSwapchainImage); XR_PROC(xrReleaseSwapchainImage);
    XrSystemGetInfo sys = {XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrSystemId system;
    CHECK(xrGetSystem(instance, &sys, &system));
    XrGraphicsBindingVulkanKHR missing_binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    XrSessionCreateInfo missing_info = {XR_TYPE_SESSION_CREATE_INFO, &missing_binding, 0, system};
    XrSession missing_session = XR_NULL_HANDLE;
    if (xrCreateSession(instance, &missing_info, &missing_session) != XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING) return 15;
    XrGraphicsRequirementsVulkanKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    PFN_xrGetVulkanGraphicsRequirementsKHR get_req;
    CHECK(xrGetInstanceProcAddr(instance, v2 ? "xrGetVulkanGraphicsRequirements2KHR" : "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&get_req));
    CHECK(get_req(instance, system, &req));
    printf("requirements Vulkan %u.%u .. %u.%u\n", XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported),
           XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported));
    VkInstance vk_instance = VK_NULL_HANDLE;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = v2 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
    VkInstanceCreateInfo vi = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; vi.pApplicationInfo = &app;
    if (v2) {
        XR_PROC(xrCreateVulkanInstanceKHR);
        XrVulkanInstanceCreateInfoKHR ci = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
        ci.systemId = system; ci.pfnGetInstanceProcAddr = vgipa; ci.vulkanCreateInfo = &vi;
        VkResult result;
        CHECK(xrCreateVulkanInstanceKHR(instance, &ci, &vk_instance, &result)); CHECK(result);
    } else {
        XR_PROC(xrGetVulkanInstanceExtensionsKHR);
        CHECK(xrGetVulkanInstanceExtensionsKHR(instance, system, 0, &count, nullptr));
        char required[256] = {};
        CHECK(xrGetVulkanInstanceExtensionsKHR(instance, system, sizeof(required), &count, required));
        if (*required) return 4;
        CHECK(((PFN_vkCreateInstance)vgipa(VK_NULL_HANDLE, "vkCreateInstance"))(&vi, nullptr, &vk_instance));
    }
    VK_PROC(vkGetPhysicalDeviceQueueFamilyProperties); VK_PROC(vkCreateDevice); VK_PROC(vkGetDeviceQueue);
    VK_PROC(vkCreateCommandPool); VK_PROC(vkAllocateCommandBuffers); VK_PROC(vkBeginCommandBuffer);
    VK_PROC(vkEndCommandBuffer); VK_PROC(vkCmdPipelineBarrier); VK_PROC(vkCmdClearColorImage);
    VK_PROC(vkQueueSubmit); VK_PROC(vkDestroyCommandPool); VK_PROC(vkDestroyDevice); VK_PROC(vkDestroyInstance);
    VK_PROC(vkCreateBuffer); VK_PROC(vkGetBufferMemoryRequirements); VK_PROC(vkGetPhysicalDeviceMemoryProperties);
    VK_PROC(vkAllocateMemory); VK_PROC(vkBindBufferMemory); VK_PROC(vkCmdCopyImageToBuffer);
    VK_PROC(vkMapMemory); VK_PROC(vkUnmapMemory); VK_PROC(vkDestroyBuffer); VK_PROC(vkFreeMemory);
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    if (v2) {
        XR_PROC(xrGetVulkanGraphicsDevice2KHR);
        XrVulkanGraphicsDeviceGetInfoKHR gi = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, nullptr, system, vk_instance};
        CHECK(xrGetVulkanGraphicsDevice2KHR(instance, &gi, &physical));
    } else {
        XR_PROC(xrGetVulkanGraphicsDeviceKHR);
        CHECK(xrGetVulkanGraphicsDeviceKHR(instance, system, vk_instance, &physical));
        XR_PROC(xrGetVulkanDeviceExtensionsKHR);
        char required[256];
        CHECK(xrGetVulkanDeviceExtensionsKHR(instance, system, 0, &count, nullptr));
        if (count > 1 && xrGetVulkanDeviceExtensionsKHR(instance, system, count - 1, &count, required) != XR_ERROR_SIZE_INSUFFICIENT) return 5;
        CHECK(xrGetVulkanDeviceExtensionsKHR(instance, system, sizeof(required), &count, required));
        if (strcmp(required, "VK_EXT_metal_objects")) return 6;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    if (family == count) return 7;
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, family, 1, &priority};
    const char *metal = "VK_EXT_metal_objects";
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    VkDevice device;
    if (v2) {
        XR_PROC(xrCreateVulkanDeviceKHR);
        XrVulkanDeviceCreateInfoKHR ci = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
        ci.systemId = system; ci.pfnGetInstanceProcAddr = vgipa; ci.vulkanPhysicalDevice = physical; ci.vulkanCreateInfo = &di;
        VkResult result;
        CHECK(xrCreateVulkanDeviceKHR(instance, &ci, &device, &result)); CHECK(result);
    } else {
        di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = &metal;
        CHECK(vkCreateDevice(physical, &di, nullptr, &device));
    }
    VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    XrGraphicsBindingVulkanKHR binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, nullptr, vk_instance, physical, device, family, 0};
    XrSessionCreateInfo si = {XR_TYPE_SESSION_CREATE_INFO, &binding, 0, system};
    XrSession session;
    binding.queueFamilyIndex = UINT32_MAX;
    if (xrCreateSession(instance, &si, &session) != XR_ERROR_GRAPHICS_DEVICE_INVALID) return 16;
    binding.queueFamilyIndex = family;
    CHECK(xrCreateSession(instance, &si, &session));
    XR_PROC(xrPollEvent); XR_PROC(xrBeginSession); XR_PROC(xrWaitFrame); XR_PROC(xrBeginFrame);
    XR_PROC(xrEndFrame); XR_PROC(xrCreateReferenceSpace); XR_PROC(xrLocateViews); XR_PROC(xrDestroySpace);
    bool ready = false;
    for (unsigned tries = 0; tries < 100 && !ready; ++tries) {
        XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
        XrResult er = xrPollEvent(instance, &event);
        if (er == XR_SUCCESS && event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            ready = ((XrEventDataSessionStateChanged *)&event)->state == XR_SESSION_STATE_READY;
        else if (er != XR_EVENT_UNAVAILABLE && er != XR_SUCCESS) return 13;
        if (!ready) Sleep(50);
    }
    if (!ready) return 14;
    XrSessionBeginInfo begin = {XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    CHECK(xrBeginSession(session, &begin));
    XrReferenceSpaceCreateInfo space_info = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; space_info.poseInReferenceSpace.orientation.w = 1;
    XrSpace space; CHECK(xrCreateReferenceSpace(session, &space_info, &space));
    CHECK(xrEnumerateSwapchainFormats(session, 0, &count, nullptr));
    std::vector<int64_t> formats(count);
    CHECK(xrEnumerateSwapchainFormats(session, count, &count, formats.data()));
    found = false; for (auto f : formats) found |= f == VK_FORMAT_R8G8B8A8_UNORM;
    if (!found) return 8;
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex = family;
    VkCommandPool pool; CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
    for (uint32_t layers : {1u, 2u}) {
        XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM; ci.sampleCount = 1; ci.width = ci.height = 64;
        ci.faceCount = ci.mipCount = 1; ci.arraySize = layers;
        XrSwapchain swap;
        ci.sampleCount = 2;
        if (xrCreateSwapchain(session, &ci, &swap) != XR_ERROR_FEATURE_UNSUPPORTED) return 17;
        ci.sampleCount = 1;
        CHECK(xrCreateSwapchain(session, &ci, &swap));
        CHECK(xrEnumerateSwapchainImages(swap, 0, &count, nullptr));
        std::vector<XrSwapchainImageVulkanKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        if (count > 1 && xrEnumerateSwapchainImages(swap, count - 1, &count, (XrSwapchainImageBaseHeader *)images.data()) != XR_ERROR_SIZE_INSUFFICIENT) return 9;
        CHECK(xrEnumerateSwapchainImages(swap, count, &count, (XrSwapchainImageBaseHeader *)images.data()));
        XrFrameWaitInfo frame_wait = {XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frame = {XR_TYPE_FRAME_STATE};
        CHECK(xrWaitFrame(session, &frame_wait, &frame));
        XrFrameBeginInfo frame_begin = {XR_TYPE_FRAME_BEGIN_INFO}; CHECK(xrBeginFrame(session, &frame_begin));
        XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO}; uint32_t index;
        CHECK(xrAcquireSwapchainImage(swap, &ai, &index));
        XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
        CHECK(xrWaitSwapchainImage(swap, &wi));
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 64*64*4*layers; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buffer; CHECK(vkCreateBuffer(device, &bi, nullptr, &buffer));
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(device, buffer, &mr);
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physical, &mp);
        uint32_t type = 0;
        VkMemoryPropertyFlags props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        while (type < mp.memoryTypeCount && (!(mr.memoryTypeBits & (1u << type)) || (mp.memoryTypes[type].propertyFlags & props) != props)) ++type;
        if (type == mp.memoryTypeCount) return 10;
        VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size, type};
        VkDeviceMemory memory; CHECK(vkAllocateMemory(device, &ma, nullptr, &memory)); CHECK(vkBindBufferMemory(device, buffer, memory, 0));
        VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(device, &ca, &cmd));
        VkCommandBufferBeginInfo cb = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd, &cb));
        VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[index].image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkClearColorValue color = {{0.25f, 0.5f, 0.75f, 1.0f}};
        vkCmdClearColorImage(cmd, images[index].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &barrier.subresourceRange);
        barrier.oldLayout = barrier.newLayout; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy = {}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers}; copy.imageExtent = {64,64,1};
        vkCmdCopyImageToBuffer(cmd, images[index].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        barrier.oldLayout = barrier.newLayout; barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
        CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        CHECK(xrReleaseSwapchainImage(swap, &ri));
        void *pixels; CHECK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &pixels));
        auto *bytes = (unsigned char *)pixels;
        for (uint32_t n = 0; n < 64*64*layers; ++n)
            if (abs(bytes[n*4]-64)>1 || abs(bytes[n*4+1]-128)>1 || abs(bytes[n*4+2]-191)>1 || bytes[n*4+3]!=255) {
                fprintf(stderr, "pixel mismatch layers=%u pixel=%u RGBA=%u,%u,%u,%u\n", layers,n,bytes[n*4],bytes[n*4+1],bytes[n*4+2],bytes[n*4+3]); return 11;
            }
        vkUnmapMemory(device, memory); vkDestroyBuffer(device, buffer, nullptr); vkFreeMemory(device, memory, nullptr);
        if (xrReleaseSwapchainImage(swap, &ri) != XR_ERROR_CALL_ORDER_INVALID) return 12;
        XrViewLocateInfo locate = {XR_TYPE_VIEW_LOCATE_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, frame.predictedDisplayTime, space};
        XrViewState view_state = {XR_TYPE_VIEW_STATE}; XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t view_count;
        CHECK(xrLocateViews(session, &locate, &view_state, 2, &view_count, views));
        XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for (unsigned eye = 0; eye < 2; ++eye) {
            pv[eye].pose = views[eye].pose; pv[eye].fov = views[eye].fov;
            pv[eye].subImage = {swap, {{0,0},{64,64}}, layers == 2 ? eye : 0};
        }
        XrCompositionLayerProjection projection = {XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr, 0, space, 2, pv};
        const XrCompositionLayerBaseHeader *layer = (XrCompositionLayerBaseHeader *)&projection;
        XrFrameEndInfo end = {XR_TYPE_FRAME_END_INFO, nullptr, frame.predictedDisplayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1, &layer};
        CHECK(xrEndFrame(session, &end));
        CHECK(xrDestroySwapchain(swap));
        printf("PASS Vulkan%s imported %u-layer Metal swapchain, all pixels, release fence and call order\n", v2 ? "2" : "", layers);
    }
    CHECK(xrDestroySpace(space));
    CHECK(xrDestroySession(session));
    vkDestroyCommandPool(device, pool, nullptr); vkDestroyDevice(device, nullptr); vkDestroyInstance(vk_instance, nullptr);
    CHECK(xrDestroyInstance(instance));
    return 0;
}
