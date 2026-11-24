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

TEST(BackboarddEnvironment, Ios17KeepsItsEnvironmentUnchanged)
{
    detach_ready = false;
    EXPECT_EQ(envFor(xnu17::native_switch_exec_environment, "/usr/libexec/backboardd"),
              (std::vector<std::string>{"CA_DISABLE_DETACHED=1"}));
    detach_ready = true;
    EXPECT_TRUE(envFor(xnu17::native_switch_exec_environment, "/usr/libexec/backboardd").empty());
}

TEST(BackboarddEnvironment, OtherProcessesKeepTheirOwnEnvironment)
{
    detach_ready = false;
    EXPECT_TRUE(envFor(xnu27::native_switch_exec_environment, "/sbin/launchd").empty());
    EXPECT_EQ(envFor(xnu27::native_switch_exec_environment, "/usr/sbin/bluetoothd"),
              (std::vector<std::string>{"BT_HCI_TRANSPORT=H4BC:POSIX:com.apple.uart.bluetooth,3000000"}));
}

}  // namespace
