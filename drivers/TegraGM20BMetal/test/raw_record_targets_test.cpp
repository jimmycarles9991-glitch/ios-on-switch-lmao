// SPDX-License-Identifier: MIT
// A record's colour targets: raw_emit writes every target's per-target blend switch, target 0's too; a two-plane state's alpha
// targets are its 550 colours' alpha indices (raw_xr_pass.h), raw_emit lays them out, and assembleRecord keeps their
// registers as it keeps those of the colours the key names, a target neither names left out (raw_record_parts.cpp foreign)
#include <gtest/gtest.h>
#include <array>
#include <string>
#include <vector>
#include "../raw/runtime/raw_emit.h"
#include "../raw/runtime/raw_record_parts.h"
#include "../raw/runtime/raw_xr_pass.h"

namespace {

constexpr uint32_t kCtWrite = 0x1a00, kBlendPerTarget = 0x1e00;
constexpr char kTwoPlaneKey[] = "samples=1 topo=0 ds=0/0 c0=550:15:0:1:0:0:1:0:0";

uint32_t registerOf(const std::vector<raw::EmitRegister> &regs, uint32_t method)
{
    for (const raw::EmitRegister &r : regs)
        if (r.method == method)
            return r.value;
    return 0xffffffffu;
}

TEST(RecordTargets, TheAlphaTargetsAreTheAlphaIndices)
{
    const uint32_t renderBox[8] = {550, 105, 550};
    EXPECT_EQ(raw::xrAlphaTargets(renderBox), 1u << 3 | 1u << 4);
    const uint32_t hole[8] = {550, 0, 80};
    EXPECT_EQ(raw::xrAlphaTargets(hole), 1u << 1);
    const uint32_t after[8] = {0, 550};
    EXPECT_EQ(raw::xrAlphaTargets(after), 1u << 0);
    const uint32_t full[8] = {550, 80, 80, 80, 80, 80, 80, 80};
    EXPECT_EQ(raw::xrAlphaTargets(full), 0u);
}

TEST(RecordTargets, EmitStateLaysOutTheAlphaTargets)
{
    const std::array<uint8_t, 80> header{};
    raw::EmitProgram vertex, fragment;
    vertex.header = header.data();
    fragment.header = header.data();
    std::vector<raw::EmitRegister> regs;
    std::string why;
    uint32_t alpha = ~0u;
    ASSERT_TRUE(raw::emitState(kTwoPlaneKey, vertex, fragment, regs, why, nullptr, true, &alpha)) << why;
    EXPECT_EQ(alpha, 1u << 1);
    EXPECT_NE(registerOf(regs, kCtWrite + 4), 0xffffffffu);
    ASSERT_TRUE(raw::emitState(kTwoPlaneKey, vertex, fragment, regs, why, nullptr, false, &alpha)) << why;
    EXPECT_EQ(alpha, 0u);
}

TEST(RecordTargets, EveryTargetBlendsAlphaByItsOwnFactors)
{
    const std::array<uint8_t, 80> header{};
    raw::EmitProgram vertex, fragment;
    vertex.header = header.data();
    fragment.header = header.data();
    std::vector<raw::EmitRegister> regs;
    std::string why;
    ASSERT_TRUE(raw::emitState("samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0 c1=80:15:0:1:0:0:1:0:0", vertex,
                               fragment, regs, why))
        << why;
    EXPECT_EQ(registerOf(regs, kBlendPerTarget), 1u);
    EXPECT_EQ(registerOf(regs, kBlendPerTarget + 0x20), 1u);
    EXPECT_NE(raw::emittableMethods().count(kBlendPerTarget), 0u);
}

raw::CapturedProgram program(const char *id)
{
    raw::CapturedProgram p;
    p.id = id;
    p.image.assign(96, 0);
    p.cbufTypes = {raw::CbufRoot};
    return p;
}

// The blob of a state of one 550 colour at target 0 whose registers name targets 0, 1 and 2
std::vector<uint32_t> blobOf(uint32_t alphaTargets)
{
    static const raw::CapturedProgram vs = program("vs"), fs = program("fs");
    raw::RecordParts parts;
    parts.programs[0] = &vs;
    parts.programs[1] = &fs;
    parts.regs = {{kCtWrite, 0x1111}, {kCtWrite + 4, 0x1000}, {kCtWrite + 8, 0x1111}, {kBlendPerTarget + 0x20, 1}};
    parts.alphaTargets = alphaTargets;
    raw::PackRules rules;
    rules.varying = {kCtWrite, kCtWrite + 4, kCtWrite + 8, kBlendPerTarget + 0x20};
    raw::RuntimeRecord out;
    std::string why;
    EXPECT_TRUE(raw::assembleRecord(parts, std::string(kTwoPlaneKey) + " | v vs | f fs", rules, out, why)) << why;
    EXPECT_TRUE(out.extras.empty());
    return out.blob;
}

TEST(RecordTargets, ARecordKeepsItsAlphaTargetsRegisters)
{
    EXPECT_EQ(blobOf(1u << 1),
              (std::vector<uint32_t>{kCtWrite, 0x1111, kCtWrite + 4, 0x1000, kBlendPerTarget + 0x20, 1}));
    EXPECT_EQ(blobOf(0), (std::vector<uint32_t>{kCtWrite, 0x1111}));
}

}  // namespace
