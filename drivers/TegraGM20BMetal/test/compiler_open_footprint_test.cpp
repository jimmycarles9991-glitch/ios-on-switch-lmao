// SPDX-License-Identifier: MIT
// What the system shader compiler's open costs before it serves anything (compiler_engine.mm openDevice), each step in a
// process of its own over hostsim's mock GM20B: NVK's image; a bare NVK instance and device (what NAK's compile and the
// direct compile's pipeline layouts need); and the m2v device the compiler opens through native_bridge_metal_open (the
// m2v runtime over that device). Each step's growth is named by image and VM tag (footprint_breakdown.h).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#include "footprint_breakdown.h"
#include "bazel/testing/fresh_process.h"
#include <vulkan/vulkan_core.h>

#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

std::string dir()
{
    const char *tmp = getenv("TEST_TMPDIR");
    return tmp ? tmp : "/tmp";
}

void note(const char *step, const std::string &line)
{
    std::ofstream(dir() + "/" + step + ".txt", std::ios::app) << line << "\n";
}

void show(const char *step)
{
    std::ifstream in(dir() + "/" + step + ".txt");
    for (std::string line; std::getline(in, line);)
        printf("%s: %s\n", step, line.c_str());
}

std::string loader()
{
    char path[PATH_MAX];
    const char *nvk = getenv("SWITCH_NVK");
    return nvk && realpath(nvk, path) ? path : "";
}

// The compiler's own environment (compiler_engine.mm Engine::open)
void compilerEnvironment()
{
    setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
    setenv("NVK_DARWIN_SERVICE", "AGXAccelerator", 0);
    setenv("NMB_SHADOW_LOG", "0", 0);
}

struct Step {
    const char *name;
    int64_t footprint = footprint::physFootprint();
    std::map<std::string, int64_t> owners = footprint::dirtyByOwner();

    void report(const char *what)
    {
        const int64_t now = footprint::physFootprint();
        const std::map<std::string, int64_t> after = footprint::dirtyByOwner();
        char head[160];
        snprintf(head, sizeof head, "%s %+lld KB (footprint %lld KB):", what, (long long)(now - footprint) / 1024,
                 (long long)now / 1024);
        note(name, head + footprint::growth(owners, after));
        footprint = now;
        owners = after;
    }
};

void *nvkImage()
{
    return dlopen(loader().c_str(), RTLD_NOW | RTLD_LOCAL);
}

void launch()
{
    Step s{"launch"};
    note("launch", "at launch " + std::to_string(s.footprint / 1024) +
                       " KB:" + footprint::growth({}, footprint::dirtyByOwner(), 16));
}

void bareNvkDevice()
{
    compilerEnvironment();
    Step s{"nvk_device"};
    void *nvk = nvkImage();
    ASSERT_NE(nvk, nullptr) << dlerror();
    s.report("NVK's image");
    const auto icd = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(nvk, "vk_icdGetInstanceProcAddr"));
    ASSERT_NE(icd, nullptr);
    const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(icd(nullptr, "vkCreateInstance"));
    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instanceInfo = {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    ASSERT_EQ(createInstance(&instanceInfo, nullptr, &instance), VK_SUCCESS);
    const auto enumerate =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(icd(instance, "vkEnumeratePhysicalDevices"));
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    ASSERT_GE(enumerate(instance, &count, &physical), VK_SUCCESS);
    ASSERT_EQ(count, 1u);
    s.report("an instance and its physical device");
    const float priority = 1;
    VkDeviceQueueCreateInfo queue = {};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo = {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queue;
    VkDevice device = VK_NULL_HANDLE;
    const auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(icd(instance, "vkCreateDevice"));
    ASSERT_EQ(createDevice(physical, &deviceInfo, nullptr, &device), VK_SUCCESS);
    s.report("a device of one queue");
}

void m2vDevice(const char *step)
{
    compilerEnvironment();
    Step s{step};
    ASSERT_NE(nvkImage(), nullptr) << dlerror();
    s.report("NVK's image");
    id device = nil;
    @autoreleasepool {
        device = native_bridge_metal_open(loader().c_str(), nullptr, 0, nullptr);
    }
    ASSERT_TRUE(device != nil) << nmb_last_error();
    s.report("the m2v device");
    native_bridge_metal_release_idle_memory();
    s.report("after its idle release");
}

}  // namespace

TEST(CompilerOpen, AtLaunch)
{
    switch_ios::test::ExpectPassesInFreshProcess(launch);
    show("launch");
}

TEST(CompilerOpen, ABareNvkDevice)
{
    switch_ios::test::ExpectPassesInFreshProcess(bareNvkDevice);
    show("nvk_device");
}

TEST(CompilerOpen, TheM2VDevice)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        m2vDevice("m2v_device");
    });
    show("m2v_device");
}

// The runtime's own sizes at their least (a client's memo, its smallest pool blocks): what of the open they decide
TEST(CompilerOpen, TheM2VDeviceAtItsLeast)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        setenv("NMB_MEMO_MB", "1", 1);
        setenv("M2V_BLOCK_MIB", "1", 1);
        setenv("NMB_IDLE_RELEASE_MS", "0", 1);
        m2vDevice("m2v_device_least");
    });
    show("m2v_device_least");
}
