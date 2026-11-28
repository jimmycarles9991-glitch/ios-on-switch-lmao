// SPDX-License-Identifier: MIT
// The sample pack's header names its rules as the shader compiler reads them: the 16 bytes packfile.py rules_identity
// wrote are the SHA-256 of the rules PackRules::of derives from the pack, as little-endian words, each list's count
// first. The compiler names its store by those bytes, never deriving them, so the writer's derivation and the runtime's
// must agree.
#include <CommonCrypto/CommonDigest.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "drivers/TegraGM20BMetal/pack.h"
#include "drivers/TegraGM20BMetal/record.h"

namespace {

std::string packPath()
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, before any test thread starts
    const char *path = std::getenv("SWITCH_SAMPLE_PACK");
    return path ? path : "";
}

std::vector<uint8_t> canonical(const gm20b::PackRules &r)
{
    std::vector<uint8_t> bytes;
    const auto word = [&](uint32_t v) {
        for (unsigned i = 0; i < 4; ++i)
            bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    word(static_cast<uint32_t>(r.varying.size()));
    for (const uint32_t v : r.varying)
        word(v);
    for (const auto *registers : {&r.baseline, &r.context}) {
        word(static_cast<uint32_t>(registers->size()));
        for (const auto &[method, value] : *registers) {
            word(method);
            word(value);
        }
    }
    return bytes;
}

TEST(PackRulesIdentity, TheHeaderNamesTheRulesTheRuntimeDerives)
{
    gm20b::Pack pack;
    std::string why;
    ASSERT_TRUE(pack.load(packPath().c_str(), 0, why)) << why;
    const gm20b::PackRules rules = gm20b::PackRules::of(pack);
    ASSERT_FALSE(rules.varying.empty());
    const std::vector<uint8_t> bytes = canonical(rules);
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest);
    EXPECT_EQ(memcmp(pack.header()->rules, digest, sizeof pack.header()->rules), 0);
}

}  // namespace
