#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../switch/include/switch/app_code_rules.h"
#include "../switch/include/switch/app_code_macho.h"
#include "../switch/include/switch/dyld_policy_24B119.h"

namespace {
void put32(std::vector<uint8_t>& data, size_t at, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) data.at(at + i) = static_cast<uint8_t>(value >> (8 * i));
}
void put64(std::vector<uint8_t>& data, size_t at, uint64_t value)
{
    put32(data, at, static_cast<uint32_t>(value));
    put32(data, at + 4, static_cast<uint32_t>(value >> 32));
}
void big32(std::vector<uint8_t>& data, size_t at, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) data.at(at + i) = static_cast<uint8_t>(value >> (24 - 8 * i));
}
std::vector<uint8_t> image()
{
    std::vector<uint8_t> data(120);
    put32(data, 0, 0xfeedfacf);
    put32(data, 4, 0x0100000c);
    put32(data, 12, 6);
    put32(data, 16, 2);
    put32(data, 20, 88);
    put32(data, 32, 0x19);
    put32(data, 36, 72);
    put64(data, 72, 0);
    put64(data, 80, 0x1000);
    put32(data, 92, 5);
    put32(data, 104, 0x1b);
    put32(data, 108, 16);
    return data;
}
}

TEST(SwitchAppCode, ContainsEveryExecutableDescendantWithoutNamingRules)
{
    const std::string root = "/private/var/containers/Bundle/Application/UUID/Anything.app";
    for (const auto* file : {"/main", "/X/Y/custom.helper/run", "/Other/nested.appex/main", "/lib.dylib"}) {
        EXPECT_EQ(switch_app_code_bundle_end((root + file).c_str()), root.size());
    }
    EXPECT_EQ(switch_app_code_bundle_end("/var/containers/Bundle/Application/C/B/A"), 38u);
    for (const auto* path : {"/Applications/Anything.app/main", "/private/var/mobile/Anything.app/main",
                             "/private/var/containers/Bundle/ApplicationToo/C/B/main",
                             "/private/var/containers/Bundle/Application/C/B",
                             "/private/var/containers/Bundle/Application//B/main",
                             "/private/var/containers/Bundle/Application/../B/main"}) {
        EXPECT_EQ(switch_app_code_bundle_end(path), 0u) << path;
    }
}

TEST(SwitchAppCode, MarkerCannotCarryPermissionsOrRedirect)
{
    EXPECT_TRUE(switch_app_code_marker_valid(true, 0100600, 0, 1));
    EXPECT_FALSE(switch_app_code_marker_valid(false, 0600, 0, 1));
    EXPECT_FALSE(switch_app_code_marker_valid(true, 0644, 0, 1));
    EXPECT_FALSE(switch_app_code_marker_valid(true, 04600, 0, 1));
    EXPECT_FALSE(switch_app_code_marker_valid(true, 0600, 1, 1));
    EXPECT_FALSE(switch_app_code_marker_valid(true, 0600, 0, 2));
    EXPECT_TRUE(switch_app_code_amfi("AMFI"));
    EXPECT_FALSE(switch_app_code_amfi("Sandbox"));
    EXPECT_FALSE(switch_app_code_amfi("AMFI-extra"));
    EXPECT_FALSE(switch_app_code_amfi(nullptr));
}

TEST(SwitchAppCode, MissingOrBrokenSignatureDoesNotChangeCodeCoverage)
{
    auto data = image();
    uint64_t coverage = 0;
    ASSERT_TRUE(switch_app_code_macho_coverage(data.data(), data.size(), 0x1000, &coverage));
    EXPECT_EQ(coverage, 0x1000u);
    put32(data, 104, 0x1d);
    put32(data, 112, 0xffffffff);
    put32(data, 116, 0xffffffff);
    ASSERT_TRUE(switch_app_code_macho_coverage(data.data(), data.size(), 0x1000, &coverage));
    EXPECT_EQ(coverage, 0x1000u);
}

TEST(SwitchAppCode, DyldPolicyRequiresPinnedImageAndExecutableSite)
{
    auto data = image();
    data.resize(128);
    put32(data, 12, 7);
    put32(data, 20, 96);
    put64(data, 56, 0x100000000);
    put64(data, 80, 0x40000);
    put32(data, 108, 24);
    const std::array<uint8_t, 16> uuid = {
        0x17, 0xf0, 0x28, 0x09, 0xe0, 0xe1, 0x32, 0xf4,
        0x86, 0x2e, 0xfa, 0xcd, 0xe8, 0x62, 0xb8, 0xde,
    };
    std::memcpy(data.data() + 112, uuid.data(), uuid.size());
    uint64_t address = 0;
    ASSERT_TRUE(switch_dyld_policy_24B119_site(data.data(), data.size(), 0x8000, &address));
    EXPECT_EQ(address, 0x1000362c8u);
    EXPECT_FALSE(switch_dyld_policy_24B119_site(data.data(), data.size() - 1, 0, &address));
    data[112] ^= 1;
    EXPECT_FALSE(switch_dyld_policy_24B119_site(data.data(), data.size(), 0, &address));
    data[112] ^= 1;
    put32(data, 92, 1);
    EXPECT_FALSE(switch_dyld_policy_24B119_site(data.data(), data.size(), 0, &address));
    put32(data, 92, 5);
    EXPECT_FALSE(switch_dyld_policy_24B119_site(data.data(), data.size(), UINT64_MAX, &address));
}

TEST(SwitchAppCode, DyldPolicyPreservesLibraryValidationTarget)
{
    uint32_t words[] = {0xf94006a0, 0x3945ec08, 0x3707dba8};
    ASSERT_TRUE(switch_dyld_policy_24B119_patch(words));
    const uint32_t immediate = words[2] & 0x3ffffff;
    const int64_t displacement = static_cast<int64_t>(immediate) - (int64_t{1} << 26);
    EXPECT_EQ(0x2e2d0 + displacement * 4, 0x2de44);
    EXPECT_TRUE(switch_dyld_policy_24B119_patch(words));
    words[0] ^= 1;
    EXPECT_FALSE(switch_dyld_policy_24B119_patch(words));
}

TEST(SwitchAppCode, RejectsTruncationAndInvalidExecutableBounds)
{
    auto original = image();
    uint64_t coverage = 0;
    for (size_t size = 0; size < original.size(); ++size)
        EXPECT_FALSE(switch_app_code_macho_coverage(original.data(), size, 0x1000, &coverage));
    for (const auto& edit : std::array<std::pair<size_t, uint32_t>, 8>{
             {{0, 0}, {4, 7}, {16, 0}, {20, 0}, {20, UINT32_MAX}, {36, 7}, {36, 128}, {96, 1}}}) {
        auto data = original;
        put32(data, edit.first, edit.second);
        EXPECT_FALSE(switch_app_code_macho_coverage(data.data(), data.size(), 0x1000, &coverage));
    }
    put64(original, 72, 0xff0);
    EXPECT_FALSE(switch_app_code_macho_coverage(original.data(), original.size(), 0x1000, &coverage));
}

TEST(SwitchAppCode, FatImageCoverageCannotReachAnotherArchitecture)
{
    std::vector<uint8_t> fat(48);
    big32(fat, 0, 0xcafebabe);
    big32(fat, 4, 2);
    big32(fat, 16, 0x1000);
    big32(fat, 20, 0x1000);
    big32(fat, 36, 0x2000);
    big32(fat, 40, 0x1000);
    uint64_t size = 0, coverage = 0;
    ASSERT_TRUE(switch_app_code_slice_size(fat.data(), fat.size(), 0x3000, 0x1000, &size));
    EXPECT_EQ(size, 0x1000u);
    auto data = image();
    put64(data, 80, 0x1001);
    EXPECT_FALSE(switch_app_code_macho_coverage(data.data(), data.size(), size, &coverage));
    EXPECT_FALSE(switch_app_code_slice_size(fat.data(), fat.size(), 0x3000, 0x1001, &size));
    EXPECT_FALSE(switch_app_code_slice_size(fat.data(), fat.size() - 1, 0x3000, 0x1000, &size));
    big32(fat, 36, 0x1800);
    EXPECT_FALSE(switch_app_code_slice_size(fat.data(), fat.size(), 0x3000, 0x1000, &size));
}

TEST(SwitchAppCode, ThinSliceStartsAtZeroAndFat64IsBounded)
{
    const auto data = image();
    uint64_t size = 0;
    EXPECT_TRUE(switch_app_code_slice_size(data.data(), 8, 0x1000, 0, &size));
    EXPECT_EQ(size, 0x1000u);
    EXPECT_FALSE(switch_app_code_slice_size(data.data(), 8, 0x1000, 1, &size));
    std::vector<uint8_t> fat(40);
    big32(fat, 0, 0xcafebabf);
    big32(fat, 4, 1);
    big32(fat, 20, 0x1000);
    big32(fat, 28, 0x1000);
    EXPECT_TRUE(switch_app_code_slice_size(fat.data(), fat.size(), 0x2000, 0x1000, &size));
    EXPECT_FALSE(switch_app_code_slice_size(fat.data(), fat.size(), 0x1fff, 0x1000, &size));
    big32(fat, 4, UINT32_MAX);
    EXPECT_EQ(switch_app_code_fat_bytes(fat.data(), fat.size()), 0u);
}
