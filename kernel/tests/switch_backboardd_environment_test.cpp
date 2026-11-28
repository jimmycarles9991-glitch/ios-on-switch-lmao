// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char EXECUTABLE_KEY[] = "executable_path=";
bool detach_ready = false;

struct image_params {
    const char *ip_strings;
};

bool native_switch_disabled(const char *)
{
    return false;
}

bool native_legacy_detach_ready()
{
    return detach_ready;
}

namespace xnu17 {
#include "backboardd_environment_17.h"
}  // namespace xnu17

namespace xnu27 {
#include "backboardd_environment_27.h"
}  // namespace xnu27

std::vector<std::string> collect(const char *const *env)
{
    std::vector<std::string> out;
    for (; env && *env; ++env)
        out.emplace_back(*env);
    return out;
}

template <typename Fn> std::vector<std::string> envFor(Fn fn, const char *path)
{
    const std::string strings = std::string(EXECUTABLE_KEY) + path;
    const image_params imgp{strings.c_str()};
    return collect(fn(&imgp));
}

TEST(BackboarddEnvironment, Ios27DisablesGenericShadersWithAndWithoutDetached)
{
    detach_ready = false;
    EXPECT_EQ(envFor(xnu27::native_switch_exec_environment, "/usr/libexec/backboardd"),
              (std::vector<std::string>{"CA_DISABLE_DETACHED=1", "CA_DISABLE_GENERIC_SHADERS=1"}));
    detach_ready = true;
    EXPECT_EQ(envFor(xnu27::native_switch_exec_environment, "/usr/libexec/backboardd"),
              (std::vector<std::string>{"CA_DISABLE_GENERIC_SHADERS=1"}));
}

TEST(BackboarddEnvironment, Ios17DisablesGenericShadersWithAndWithoutDetached)
{
    detach_ready = false;
    EXPECT_EQ(envFor(xnu17::native_switch_exec_environment, "/usr/libexec/backboardd"),
              (std::vector<std::string>{"CA_DISABLE_DETACHED=1", "CA_DISABLE_GENERIC_SHADERS=1"}));
    detach_ready = true;
    EXPECT_EQ(envFor(xnu17::native_switch_exec_environment, "/usr/libexec/backboardd"),
              (std::vector<std::string>{"CA_DISABLE_GENERIC_SHADERS=1"}));
}

// Any process may render Core Animation in-process (CARenderer, snapshots, widgets): none draws the catch-all
TEST(BackboarddEnvironment, EveryOtherProcessDisablesGenericShaders)
{
    detach_ready = false;
    const std::vector<std::string> ca = {"CA_DISABLE_GENERIC_SHADERS=1"};
    const std::vector<std::string> bluetoothd = {"BT_HCI_TRANSPORT=H4BC:POSIX:com.apple.uart.bluetooth,3000000",
                                                 "CA_DISABLE_GENERIC_SHADERS=1"};
    for (const auto fn : {xnu17::native_switch_exec_environment, xnu27::native_switch_exec_environment}) {
        EXPECT_EQ(envFor(fn, "/sbin/launchd"), ca);
        EXPECT_EQ(envFor(fn, "/System/Library/CoreServices/SpringBoard.app/SpringBoard"), ca);
        EXPECT_EQ(envFor(fn, "/Applications/SwitchHWTest.app/SwitchHWTest"), ca);
        EXPECT_EQ(envFor(fn, "/usr/sbin/bluetoothd"), bluetoothd);
    }
}

// A process that passes its own environment on keeps one copy of each variable the kernel adds, whatever its value
TEST(BackboarddEnvironment, TheKernelAddsNoVariableTheProcessAlreadyNames)
{
    const char strings[] = "HOME=/var/mobile\0CA_DISABLE_GENERIC_SHADERS=0\0CA_DISABLE_DETACHEDX=1";
    const char *end = strings + sizeof strings;
    for (const auto holds : {xnu17::native_switch_environment_holds, xnu27::native_switch_environment_holds}) {
        EXPECT_TRUE(holds(strings, end, "CA_DISABLE_GENERIC_SHADERS=1"));
        EXPECT_FALSE(holds(strings, end, "CA_DISABLE_DETACHED=1")) << "a longer name is another variable";
        EXPECT_FALSE(holds(strings, end, "BT_HCI_TRANSPORT=H4BC"));
        EXPECT_FALSE(holds(strings, strings, "HOME=/"));
    }
}

}  // namespace
