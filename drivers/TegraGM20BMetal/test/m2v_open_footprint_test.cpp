// SPDX-License-Identifier: MIT
// What opening the m2v device costs a client process, which a process the raw path serves alone no longer pays (RawM2V):
// the frontend over the host bridge and NVK's host twin over hostsim's mock GM20B (mock_iokit.c, linked: its interposers
// stand in for IOKit's), opened as the plugin opens it outside the render server. NVK's image is loaded first and measured
// on its own: the raw path takes NVK's connection, so every process still loads it.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#include "../hostsim/mock_iokit.h"

#include <dlfcn.h>
#include <gtest/gtest.h>
#include <mach/mach.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int64_t footprint()
{
    task_vm_info_data_t vm = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count);
    return int64_t(vm.phys_footprint);
}

void missing(const char *cls, const char *selector)
{
    fprintf(stderr, "missing -[%s %s]\n", cls, selector);
}

}  // namespace

TEST(M2VOpen, ClientFootprint)
{
    const char *nvk = getenv("SWITCH_NVK");
    ASSERT_NE(nvk, nullptr);
    char loader[PATH_MAX];
    ASSERT_NE(realpath(nvk, loader), nullptr);
    const std::string cache = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/m2v";
    mkdir(cache.c_str(), 0755);
    setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
    setenv("NMB_MEMO_MB", "32", 0);
    setenv("M2V_BLOCK_MIB", "4", 0);
    setenv("NMB_SYSTEM_COMPILER", "1", 0);
    const int64_t start = footprint();
    ASSERT_NE(dlopen(loader, RTLD_NOW | RTLD_LOCAL), nullptr) << dlerror();
    const int64_t loaded = footprint();
    id device = nil;
    @autoreleasepool {
        device = native_bridge_metal_open(loader, cache.c_str(), 0, missing);
    }
    ASSERT_TRUE(device != nil) << nmb_last_error();
    const int64_t opened = footprint();
    mock_memory m = {};
    mock_iokit_memory(&m);
    native_bridge_metal_release_idle_memory();
    const int64_t settled = footprint();
    mock_memory s = {};
    mock_iokit_memory(&s);
    // On the device a cached BO's mapping counts whole in phys_footprint (iokit_mapped) and a coherent one in no ledger;
    // here both are this process's anonymous memory
    const int64_t backing = int64_t(s.live.cached + s.live.coherent), charged = int64_t(s.mapped.cached);
    printf("m2v open: NVK image %+lld KB; device open %+lld KB (GPU BOs %llu KB cached + %llu KB coherent, %llu KB of "
           "them mapped cached); after its idle release %+lld KB over the image, as iOS charges it %+lld KB\n",
           (long long)(loaded - start) / 1024, (long long)(opened - loaded) / 1024,
           (unsigned long long)(m.live.cached >> 10), (unsigned long long)(m.live.coherent >> 10),
           (unsigned long long)(m.mapped.cached >> 10), (long long)(settled - loaded) / 1024,
           (long long)(settled - loaded - backing + charged) / 1024);
    EXPECT_GT(settled, loaded);
}
