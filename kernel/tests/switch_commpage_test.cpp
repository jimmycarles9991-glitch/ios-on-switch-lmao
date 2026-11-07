// <switch/commpage.h>: the ARMv8 crypto extensions from ID_AA64ISAR0_EL1 (the Cortex-A57's value, and fields cleared
// one at a time), and its hook in both trees' commpage_init_arm_optional_features_isar0.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#define ID_AA64ISAR0_EL1_SHA2_MASK    (0xfull << 12)
#define ID_AA64ISAR0_EL1_SHA2_EN      (1ull << 12)
#define ID_AA64ISAR0_EL1_SHA1_MASK    (0xfull << 8)
#define ID_AA64ISAR0_EL1_SHA1_EN      (1ull << 8)
#define ID_AA64ISAR0_EL1_AES_MASK     (0xfull << 4)
#define ID_AA64ISAR0_EL1_AES_EN       (1ull << 4)
#define ID_AA64ISAR0_EL1_AES_PMULL_EN (2ull << 4)
#define kHasARMv8Crypto               0x01000000

int gARM_FEAT_AES, gARM_FEAT_PMULL, gARM_FEAT_SHA1, gARM_FEAT_SHA256;

#include <switch/commpage.h>

using testing::HasSubstr;

namespace {

// Cortex-A57 TRM: ID_AA64ISAR0_EL1 is 0x11120 with the Cryptographic Extension (CRC32 1, SHA2 1, SHA1 1, AES 2).
constexpr uint64_t kCortexA57 = 0x11120;

std::string Text(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

TEST(SwitchCommpage, TheA57ReportsAllFourCryptoFeatures)
{
    EXPECT_EQ(switch_commpage_crypto_bits(kCortexA57), static_cast<uint64_t>(kHasARMv8Crypto));
    EXPECT_EQ(gARM_FEAT_AES, 1);
    EXPECT_EQ(gARM_FEAT_PMULL, 1);
    EXPECT_EQ(gARM_FEAT_SHA1, 1);
    EXPECT_EQ(gARM_FEAT_SHA256, 1);
}

TEST(SwitchCommpage, AMissingFeatureClearsItsSysctlAndTheCommpageBit)
{
    EXPECT_EQ(switch_commpage_crypto_bits(kCortexA57 & ~ID_AA64ISAR0_EL1_SHA2_MASK), 0u);
    EXPECT_EQ(gARM_FEAT_SHA256, 0);
    EXPECT_EQ(gARM_FEAT_AES, 1);

    EXPECT_EQ(switch_commpage_crypto_bits((kCortexA57 & ~ID_AA64ISAR0_EL1_AES_MASK) | ID_AA64ISAR0_EL1_AES_EN), 0u);
    EXPECT_EQ(gARM_FEAT_AES, 1);
    EXPECT_EQ(gARM_FEAT_PMULL, 0);

    EXPECT_EQ(switch_commpage_crypto_bits(0), 0u);
    EXPECT_EQ(gARM_FEAT_AES + gARM_FEAT_PMULL + gARM_FEAT_SHA1 + gARM_FEAT_SHA256, 0);
}

TEST(SwitchCommpage, BothTreesAddTheBitsFromISAR0)
{
    for (const char *path : {COMMPAGE_10063, COMMPAGE_12377}) {
        const std::string s = Text(path);
        const size_t fn = s.find("\ncommpage_init_arm_optional_features_isar0(uint64_t *commpage_bits)\n");
        ASSERT_NE(fn, std::string::npos) << path;
        const std::string body = s.substr(fn, s.find("\n}\n", fn) - fn);
        const size_t hook = body.find("\tbits |= switch_commpage_crypto_bits(isar0);\n");
        ASSERT_NE(hook, std::string::npos) << path;
        EXPECT_LT(hook, body.find("*commpage_bits |= bits;")) << path;
        EXPECT_THAT(s, HasSubstr("#include <switch/commpage.h>")) << path;
    }
}

}  // namespace
