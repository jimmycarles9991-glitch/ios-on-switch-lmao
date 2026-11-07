// SPDX-License-Identifier: MIT
// The host proofs' Vulkan over NVK's host twin and raw/pack's mock GPU (nvk_alias.cpp, surface_layout.cpp): a device of its
// own per Vk (the first one of the process on the connection the raw path shares, a later one on a connection of its own, as
// another process's), NVK's memory over foreign or host memory, images bound to it as m2v's views are made (for host
// allocations), their GPU address, and the CPU's view of their texels (VK_EXT_host_image_copy, NIL's layout).
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "TegraGM20BABI.h"

extern "C" unsigned long mock_iokit_calls(unsigned method);
extern "C" unsigned long mock_iokit_kind_maps(unsigned kind);

namespace host {

inline int failures;

inline void expect(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "OK  " : "FAIL", what);
    failures += !ok;
}

struct Calls {
    unsigned long imports, allocs, binds, frees;

    static Calls now()
    {
        return {mock_iokit_calls(TegraGM20BMethodBoImport), mock_iokit_calls(TegraGM20BMethodBoAlloc),
                mock_iokit_calls(TegraGM20BMethodVmBind), mock_iokit_calls(TegraGM20BMethodBoFree)};
    }

    bool operator==(const Calls &o) const
    {
        return imports == o.imports && allocs == o.allocs && binds == o.binds && frees == o.frees;
    }
};

#define HOST_VK_FN(f) PFN_##f f = nullptr

struct Vk {
    void *nvk = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    HOST_VK_FN(vkGetMemoryHostPointerPropertiesEXT);
    HOST_VK_FN(vkAllocateMemory);
    HOST_VK_FN(vkFreeMemory);
    HOST_VK_FN(vkCreateImage);
    HOST_VK_FN(vkDestroyImage);
    HOST_VK_FN(vkGetImageMemoryRequirements);
    HOST_VK_FN(vkGetImageSubresourceLayout);
    HOST_VK_FN(vkBindImageMemory);
    HOST_VK_FN(vkTransitionImageLayoutEXT);
    HOST_VK_FN(vkCopyImageToMemoryEXT);
    HOST_VK_FN(vkCopyMemoryToImageEXT);
    HOST_VK_FN(vkCreateBuffer);
    HOST_VK_FN(vkDestroyBuffer);
    HOST_VK_FN(vkBindBufferMemory);
    HOST_VK_FN(vkGetBufferDeviceAddress);
    PFN_vkGetPhysicalDeviceImageFormatProperties2 imageFormatProperties = nullptr;
    VkPhysicalDevice physical = VK_NULL_HANDLE;

    template <class F> F symbol(const char *name) const
    {
        return reinterpret_cast<F>(dlsym(nvk, name));
    }

    bool open(const char *path)
    {
        nvk = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        auto gipa = nvk ? symbol<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr") : nullptr;
        if (!gipa)
            return false;
        const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
        VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        VkInstance instance = VK_NULL_HANDLE;
        if (createInstance(&ici, nullptr, &instance) != VK_SUCCESS)
            return false;
        const auto enumerate =
            reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
        const auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"));
        const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
        imageFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
            gipa(instance, "vkGetPhysicalDeviceImageFormatProperties2"));
        uint32_t count = 1;
        enumerate(instance, &count, &physical);
        VkPhysicalDeviceBufferDeviceAddressFeatures bda = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        bda.bufferDeviceAddress = VK_TRUE;
        VkPhysicalDeviceHostImageCopyFeaturesEXT hic = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT,
                                                        &bda};
        hic.hostImageCopy = VK_TRUE;
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueCount = 1;
        q.pQueuePriorities = &priority;
        const char *extensions[] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME};
        VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &hic};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &q;
        dci.enabledExtensionCount = 2;
        dci.ppEnabledExtensionNames = extensions;
        if (!physical || createDevice(physical, &dci, nullptr, &device) != VK_SUCCESS)
            return false;
#define HOST_VK_LOAD(f) f = reinterpret_cast<PFN_##f>(gdpa(device, #f))
        HOST_VK_LOAD(vkGetMemoryHostPointerPropertiesEXT);
        HOST_VK_LOAD(vkAllocateMemory);
        HOST_VK_LOAD(vkFreeMemory);
        HOST_VK_LOAD(vkCreateImage);
        HOST_VK_LOAD(vkDestroyImage);
        HOST_VK_LOAD(vkGetImageMemoryRequirements);
        HOST_VK_LOAD(vkGetImageSubresourceLayout);
        HOST_VK_LOAD(vkBindImageMemory);
        HOST_VK_LOAD(vkTransitionImageLayoutEXT);
        HOST_VK_LOAD(vkCopyImageToMemoryEXT);
        HOST_VK_LOAD(vkCopyMemoryToImageEXT);
        HOST_VK_LOAD(vkCreateBuffer);
        HOST_VK_LOAD(vkDestroyBuffer);
        HOST_VK_LOAD(vkBindBufferMemory);
        HOST_VK_LOAD(vkGetBufferDeviceAddress);
#undef HOST_VK_LOAD
        return true;
    }

    // NVK's memory over the host bytes [cpu, cpu + bytes), imported as m2v's no-copy buffers are
    VkDeviceMemory hostMemory(void *cpu, uint64_t bytes) const
    {
        VkMemoryHostPointerPropertiesEXT props = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        vkGetMemoryHostPointerPropertiesEXT(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, cpu,
                                            &props);
        VkImportMemoryHostPointerInfoEXT import = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        import.pHostPointer = cpu;
        VkMemoryAllocateFlagsInfo addressed = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &import};
        addressed.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &addressed};
        mai.allocationSize = bytes;
        mai.memoryTypeIndex = uint32_t(__builtin_ctz(props.memoryTypeBits ? props.memoryTypeBits : 1));
        VkDeviceMemory memory = VK_NULL_HANDLE;
        return vkAllocateMemory(device, &mai, nullptr, &memory) == VK_SUCCESS ? memory : VK_NULL_HANDLE;
    }

    // The GPU address of the memory's first byte (a buffer over it)
    uint64_t address(VkDeviceMemory memory, uint64_t bytes) const
    {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VkBuffer buffer = VK_NULL_HANDLE;
        uint64_t va = 0;
        if (vkCreateBuffer(device, &bi, nullptr, &buffer) == VK_SUCCESS &&
            vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS) {
            VkBufferDeviceAddressInfo ai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            ai.buffer = buffer;
            va = vkGetBufferDeviceAddress(device, &ai);
        }
        vkDestroyBuffer(device, buffer, nullptr);
        return va;
    }

    // An image of `format`, `w` x `h`, `levels` levels, made for host allocations as m2v's views are (after the device's format
    // query for such an image), bound to `memory` at `offset` and moved to GENERAL on the host. Null: refused.
    VkImage image(VkDeviceMemory memory, uint64_t offset, VkImageTiling tiling, VkFormat format, uint32_t w, uint32_t h,
                  uint32_t levels, VkMemoryRequirements &req) const
    {
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT;
        VkPhysicalDeviceExternalImageFormatInfo hostQuery = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        hostQuery.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkPhysicalDeviceImageFormatInfo2 query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &hostQuery};
        query.format = format;
        query.type = VK_IMAGE_TYPE_2D;
        query.tiling = tiling;
        query.usage = usage;
        VkExternalImageFormatProperties hostProps = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 props = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &hostProps};
        if (imageFormatProperties(physical, &query, &props) != VK_SUCCESS ||
            !(hostProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
            return VK_NULL_HANDLE;
        VkExternalMemoryImageCreateInfo external = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format;
        ii.extent = {w, h, 1};
        ii.mipLevels = levels;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = tiling;
        ii.usage = usage;
        VkImage image = VK_NULL_HANDLE;
        if (vkCreateImage(device, &ii, nullptr, &image) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        vkGetImageMemoryRequirements(device, image, &req);
        if (vkBindImageMemory(device, image, memory, offset) != VK_SUCCESS) {
            vkDestroyImage(device, image, nullptr);
            return VK_NULL_HANDLE;
        }
        VkHostImageLayoutTransitionInfoEXT transition = {VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO_EXT};
        transition.image = image;
        transition.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        transition.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        transition.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
        vkTransitionImageLayoutEXT(device, 1, &transition);
        return image;
    }

    // Level `level`'s w x h texels of `bpp` bytes, read (out) or written (in) on the CPU through the image's layout
    bool read(VkImage image, uint32_t level, uint32_t w, uint32_t h, uint32_t bpp, std::vector<uint8_t> &out) const
    {
        out.assign(size_t(w) * h * bpp, 0);
        VkImageToMemoryCopyEXT region = {VK_STRUCTURE_TYPE_IMAGE_TO_MEMORY_COPY_EXT};
        region.pHostPointer = out.data();
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        region.imageExtent = {w, h, 1};
        VkCopyImageToMemoryInfoEXT copy = {VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO_EXT};
        copy.srcImage = image;
        copy.srcImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        copy.regionCount = 1;
        copy.pRegions = &region;
        return vkCopyImageToMemoryEXT(device, &copy) == VK_SUCCESS;
    }

    bool write(VkImage image, uint32_t level, uint32_t w, uint32_t h, const std::vector<uint8_t> &in) const
    {
        VkMemoryToImageCopyEXT region = {VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY_EXT};
        region.pHostPointer = in.data();
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        region.imageExtent = {w, h, 1};
        VkCopyMemoryToImageInfoEXT copy = {VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO_EXT};
        copy.dstImage = image;
        copy.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        copy.regionCount = 1;
        copy.pRegions = &region;
        return vkCopyMemoryToImageEXT(device, &copy) == VK_SUCCESS;
    }
};

#undef HOST_VK_FN

}  // namespace host
