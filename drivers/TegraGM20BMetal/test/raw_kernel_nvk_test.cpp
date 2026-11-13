// SPDX-License-Identifier: MIT
// Kernel::open takes NVK's connection even while another thread is still loading NVK (the render server's m2v device open
// beside raw_metal_prepare's thread): dyld already lists the image, but RTLD_NOLOAD does not return it yet.
#include "../raw/runtime/raw_kernel.h"
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <gtest/gtest.h>
#include <cstring>
#include <string>

namespace {
constexpr char kNvkPath[] = "/System/Library/Extensions/TegraGM20BMetal.bundle/libvulkan_nouveau.dylib";
const char *const kImages[] = {"/usr/lib/libSystem.B.dylib", kNvkPath};
int nvkHandle;
uint32_t connections;
uint32_t ownServiceLookups;

bool nvkConnection(uint32_t *conn, bool *tunnel, uint32_t *selector, uint64_t *vaStart, uint64_t *vaEnd)
{
    connections++;
    *conn = 0x1234;
    *tunnel = false;
    *selector = 0;
    *vaStart = 0x100000000;
    *vaEnd = 0x200000000;
    return true;
}

void nvkConnectionRelease()
{}
}  // namespace

extern "C" uint32_t _dyld_image_count(void)
{
    return sizeof kImages / sizeof kImages[0];
}

extern "C" const char *_dyld_get_image_name(uint32_t index)
{
    return index < _dyld_image_count() ? kImages[index] : nullptr;
}

extern "C" void *dlopen(const char *path, int mode)
{
    if (mode & RTLD_NOLOAD)
        return nullptr;
    return path && !strcmp(path, kNvkPath) ? &nvkHandle : nullptr;
}

extern "C" void *dlsym(void *handle, const char *symbol)
{
    if (handle != &nvkHandle)
        return nullptr;
    if (!strcmp(symbol, "switch_nvk_connection"))
        return reinterpret_cast<void *>(&nvkConnection);
    if (!strcmp(symbol, "switch_nvk_connection_release"))
        return reinterpret_cast<void *>(&nvkConnectionRelease);
    return nullptr;
}

extern "C" io_service_t IOServiceGetMatchingService(mach_port_t, CFDictionaryRef matching)
{
    ownServiceLookups++;
    if (matching)
        CFRelease(matching);
    return 0;
}

extern "C" kern_return_t IOConnectCallStructMethod(mach_port_t, uint32_t method, const void *, size_t, void *output,
                                                   size_t *)
{
    if (method != TegraGM20BMethodGetInfo)
        return kIOReturnUnsupported;
    auto &info = *static_cast<TegraGM20BAbiInfo *>(output);
    info = {};
    info.abi_version = TegraGM20BAbiVersion;
    info.va_start = 0x1000000;
    info.va_end = 0x300000000;
    return KERN_SUCCESS;
}

TEST(RawKernelNvk, ImageStillLoadingOnAnotherThreadGivesNvksConnection)
{
    connections = ownServiceLookups = 0;
    raw::Kernel kernel;
    std::string err;
    ASSERT_TRUE(kernel.open(err)) << err;
    EXPECT_TRUE(kernel.shared());
    EXPECT_EQ(connections, 1u);
    EXPECT_EQ(ownServiceLookups, 0u);
}
