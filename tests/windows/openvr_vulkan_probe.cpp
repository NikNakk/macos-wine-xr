// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Exercise xrizer's real Vulkan backend, not just the OpenXR entry points.
#include <windows.h>
#include <vulkan/vulkan_core.h>
#include "openvr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#define CHECK(x) do { auto check_result=(x); if (check_result!=0) { fprintf(stderr,"%s: %d line %d\n",#x,(int)check_result,__LINE__); return 1; } } while(0)
#define VK_PROC(name) auto name=(PFN_##name)gipa(instance,#name); if (!name) return 2
static std::vector<const char *> extensions(std::vector<char> &text)
{
    std::vector<const char *> out;
    for (char *s=strtok(text.data()," ");s;s=strtok(nullptr," ")) out.push_back(s);
    return out;
}
int main(int argc, char **argv)
{
    HMODULE openvr=LoadLibraryA("openvr_api.dll"), vulkan=LoadLibraryA("vulkan-1.dll");
    if (!openvr || !vulkan) { fprintf(stderr,"loader error %lu\n",GetLastError()); return 2; }
    auto init=(uint32_t (*)(vr::EVRInitError *,vr::EVRApplicationType,const char *))GetProcAddress(openvr,"VR_InitInternal2");
    auto get=(void *(*)(const char *,vr::EVRInitError *))GetProcAddress(openvr,"VR_GetGenericInterface");
    auto shutdown=(void (*)())GetProcAddress(openvr,"VR_ShutdownInternal");
    auto gipa=(PFN_vkGetInstanceProcAddr)GetProcAddress(vulkan,"vkGetInstanceProcAddr");
    if (!init || !get || !shutdown || !gipa) return 2;
    vr::EVRInitError error=vr::VRInitError_None;
    init(&error,vr::VRApplication_Scene,nullptr); CHECK(error);
    auto *system=(vr::IVRSystem *)get(vr::IVRSystem_Version,&error); CHECK(error);
    auto *compositor=(vr::IVRCompositor *)get(vr::IVRCompositor_Version,&error); CHECK(error);
    if (!system || !compositor) return 3;
    std::vector<char> ie(compositor->GetVulkanInstanceExtensionsRequired(nullptr,0));
    compositor->GetVulkanInstanceExtensionsRequired(ie.data(),(uint32_t)ie.size());
    auto instance_extensions=extensions(ie);
    VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app;
    ici.enabledExtensionCount=(uint32_t)instance_extensions.size(); ici.ppEnabledExtensionNames=instance_extensions.data();
    VkInstance instance;
    CHECK(((PFN_vkCreateInstance)gipa(VK_NULL_HANDLE,"vkCreateInstance"))(&ici,nullptr,&instance));
    VK_PROC(vkGetPhysicalDeviceQueueFamilyProperties); VK_PROC(vkCreateDevice); VK_PROC(vkGetDeviceQueue);
    VK_PROC(vkCreateImage); VK_PROC(vkGetImageMemoryRequirements); VK_PROC(vkGetPhysicalDeviceMemoryProperties);
    VK_PROC(vkAllocateMemory); VK_PROC(vkBindImageMemory); VK_PROC(vkCreateCommandPool); VK_PROC(vkAllocateCommandBuffers);
    VK_PROC(vkBeginCommandBuffer); VK_PROC(vkCmdPipelineBarrier); VK_PROC(vkCmdClearColorImage); VK_PROC(vkEndCommandBuffer);
    VK_PROC(vkQueueSubmit); VK_PROC(vkQueueWaitIdle); VK_PROC(vkDestroyCommandPool); VK_PROC(vkDestroyImage);
    VK_PROC(vkFreeMemory); VK_PROC(vkDestroyDevice); VK_PROC(vkDestroyInstance);
    uint64_t raw_physical=0;
    system->GetOutputDevice(&raw_physical,vr::TextureType_Vulkan,(VkInstance_T *)instance);
    VkPhysicalDevice physical=(VkPhysicalDevice)(uintptr_t)raw_physical;
    if (!physical) return 4;
    std::vector<char> de(compositor->GetVulkanDeviceExtensionsRequired((VkPhysicalDevice_T *)physical,nullptr,0));
    compositor->GetVulkanDeviceExtensionsRequired((VkPhysicalDevice_T *)physical,de.data(),(uint32_t)de.size());
    auto device_extensions=extensions(de);
    uint32_t count=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);
    std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());
    uint32_t family=0; while (family<count && !(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT)) ++family;
    if (family==count) return 5;
    float priority=1;
    VkDeviceQueueCreateInfo qi={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,nullptr,0,family,1,&priority};
    VkDeviceCreateInfo di={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi;
    di.enabledExtensionCount=(uint32_t)device_extensions.size(); di.ppEnabledExtensionNames=device_extensions.data();
    VkDevice device; CHECK(vkCreateDevice(physical,&di,nullptr,&device));
    VkQueue queue; vkGetDeviceQueue(device,family,0,&queue);
    VkImageCreateInfo image_info={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType=VK_IMAGE_TYPE_2D; image_info.format=VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent={64,64,1}; image_info.mipLevels=image_info.arrayLayers=1; image_info.samples=VK_SAMPLE_COUNT_1_BIT;
    image_info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage image; CHECK(vkCreateImage(device,&image_info,nullptr,&image));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(device,image,&mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    uint32_t type=0; while (type<mp.memoryTypeCount && !(mr.memoryTypeBits&(1u<<type))) ++type;
    if (type==mp.memoryTypeCount) return 6;
    VkMemoryAllocateInfo ma={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,nullptr,mr.size,type};
    VkDeviceMemory memory; CHECK(vkAllocateMemory(device,&ma,nullptr,&memory)); CHECK(vkBindImageMemory(device,image,memory,0));
    VkCommandPoolCreateInfo pci={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex=family;
    VkCommandPool pool; CHECK(vkCreateCommandPool(device,&pci,nullptr,&pool));
    VkCommandBufferAllocateInfo ca={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,nullptr,pool,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(device,&ca,&cmd));
    VkCommandBufferBeginInfo cb={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd,&cb));
    VkImageMemoryBarrier barrier={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.image=image; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkClearColorValue color={{0.25f,0.5f,0.75f,1}};
    vkCmdClearColorImage(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&barrier.subresourceRange);
    barrier.oldLayout=barrier.newLayout; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit={VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cmd;
    CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    vr::VRVulkanTextureData_t data={};
    data.m_nImage=(uint64_t)image; data.m_pDevice=(VkDevice_T *)device;
    data.m_pPhysicalDevice=(VkPhysicalDevice_T *)physical; data.m_pInstance=(VkInstance_T *)instance;
    data.m_pQueue=(VkQueue_T *)queue; data.m_nQueueFamilyIndex=(argc > 1 && !strcmp(argv[1], "--unknown-queue-family")) ? UINT32_MAX : family;
    data.m_nWidth=data.m_nHeight=64; data.m_nFormat=VK_FORMAT_R8G8B8A8_UNORM; data.m_nSampleCount=1;
    vr::Texture_t texture={&data,vr::TextureType_Vulkan,vr::ColorSpace_Auto};
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    for (unsigned frame=0;frame<8;++frame) {
        CHECK(compositor->WaitGetPoses(poses,vr::k_unMaxTrackedDeviceCount,nullptr,0));
        CHECK(compositor->Submit(vr::Eye_Left,&texture)); CHECK(compositor->Submit(vr::Eye_Right,&texture));
        compositor->PostPresentHandoff();
    }
    CHECK(vkQueueWaitIdle(queue)); shutdown();
    vkDestroyCommandPool(device,pool,nullptr); vkDestroyImage(device,image,nullptr); vkFreeMemory(device,memory,nullptr);
    vkDestroyDevice(device,nullptr); vkDestroyInstance(instance,nullptr);
    puts("PASS OpenVR Vulkan device queries, xrizer backend transition and 8 stereo frame submissions");
    return 0;
}
