// bsd/vm/switch_ui_scale_sites_24B119.h's CoreMotion site: CLPlatformInfo::queryMotionCaps()'s case for hardware type
// 225 (J181), run from the site's shipped and patched bytes to the switch's common tail.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>

#include "kernel/tests/xnu_ui_scale_sites_checks.h"

using switch_ui_scale_sites::Word;

namespace {
constexpr uint64_t kCase225 = 0x1a8771e20, kTail = 0x1a8772368;
constexpr uint64_t kJ181Word = 0x0000001314618bf2;

const switch_us_site *MotionSite()
{
    return switch_ui_scale_sites::Named(switch_us_cache_24B119,
                                        "CoreMotion motion capabilities of J181 without its motion coprocessor");
}

uint64_t AddressOf(const switch_us_site &site, uint64_t fileoff)
{
    return site.page_vmaddr + (fileoff - site.page_fileoff);
}

// The instructions at each address, as the site's edits read them (patched: with what they write).
std::map<uint64_t, uint32_t> Text(const switch_us_site &site, bool patched)
{
    std::map<uint64_t, uint32_t> text;
    for (unsigned i = 0; i < site.edits; i++) {
        const switch_us_edit &edit = site.edit[i];
        uint8_t bytes[sizeof(edit.stock)] = {};
        std::memcpy(bytes, edit.stock, edit.length);
        if (patched) {
            std::memcpy(bytes + edit.at, edit.patched, edit.size);
        }
        for (unsigned at = 0; at + 4 <= edit.length; at += 4) {
            text[AddressOf(site, edit.fileoff) + at] = Word(bytes + at);
        }
    }
    return text;
}

struct CaseRun {
    uint64_t x20 = 0, x21 = 0, end = 0;
};

// movz/movk of x20/x21 (or w20), and b; anything else, or an address the edits do not hold, ends the run there.
CaseRun RunCase(const std::map<uint64_t, uint32_t> &text)
{
    CaseRun run;
    uint64_t pc = kCase225;
    for (int steps = 0; steps < 16 && pc != kTail; steps++) {
        const auto it = text.find(pc);
        if (it == text.end()) {
            break;
        }
        const uint32_t insn = it->second;
        const unsigned rd = insn & 0x1f, hw = (insn >> 21) & 3;
        const uint64_t imm16 = (insn >> 5) & 0xffff;
        uint64_t *reg = rd == 20 ? &run.x20 : rd == 21 ? &run.x21 : nullptr;
        if ((insn & 0x7f800000u) == 0x52800000u && reg != nullptr) {  // movz
            *reg = imm16 << (16 * hw);
        } else if ((insn & 0x7f800000u) == 0x72800000u && reg != nullptr) {  // movk
            *reg = (*reg & ~(0xffffULL << (16 * hw))) | imm16 << (16 * hw);
        } else if ((insn & 0xfc000000u) == 0x14000000u) {
            pc = switch_ui_scale_sites::BranchTarget(pc, insn);
            continue;
        } else {
            break;
        }
        pc += 4;
    }
    run.end = pc;
    return run;
}
}  // namespace

TEST(XnuMotionCaps24B119, ShippedCaseLoadsJ181sWord)
{
    const switch_us_site *site = MotionSite();
    ASSERT_NE(site, nullptr);
    const CaseRun run = RunCase(Text(*site, false));
    EXPECT_EQ(run.end, kTail);
    EXPECT_EQ(run.x21, kJ181Word);
    EXPECT_EQ(run.x20, 0u);
}

TEST(XnuMotionCaps24B119, PatchedCaseLoadsJ181sWordWithoutItsCoprocessorBits)
{
    const switch_us_site *site = MotionSite();
    ASSERT_NE(site, nullptr);
    const CaseRun run = RunCase(Text(*site, true));
    EXPECT_EQ(run.end, kTail);
    EXPECT_EQ(run.x20, 0u);
    EXPECT_EQ(run.x21 & ~kJ181Word, 0u) << "only clears bits";
    uint64_t cleared = 0;
    for (const unsigned bit : {1, 4, 5, 6, 7, 9, 11, 15, 16, 22, 26, 33, 36}) {
        cleared |= 1ULL << bit;
    }
    EXPECT_EQ(kJ181Word & ~run.x21, cleared);
    EXPECT_EQ(run.x21, (1ULL << 8) | (1ULL << 21) | (1ULL << 28) | (1ULL << 32));
}

TEST(XnuMotionCaps24B119, AppliedAtEveryScaleInCoreMotionsSubcache)
{
    const switch_us_site *site = MotionSite();
    ASSERT_NE(site, nullptr);
    EXPECT_TRUE(site->every_scale);
    const uint8_t subcache11[16] = {0x13, 0xd1, 0x27, 0xbc, 0x9e, 0xd2, 0x36, 0x63,
                                    0xa6, 0x80, 0xfd, 0x90, 0x2f, 0xbb, 0xd2, 0x51};
    EXPECT_EQ(std::memcmp(site->uuid, subcache11, sizeof(subcache11)), 0);
    EXPECT_EQ(AddressOf(*site, site->edit[0].fileoff), kCase225);
}

TEST(XnuMotionCaps24B119, IOS17KeepsItsMotionCapabilities)
{
    for (size_t i = 0; i < switch_us_cache_21H461.count; i++) {
        EXPECT_NE(std::string(switch_us_cache_21H461.sites[i].name), "CoreMotion");
    }
}
