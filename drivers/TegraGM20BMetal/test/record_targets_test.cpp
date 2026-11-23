// SPDX-License-Identifier: MIT
// A record's colour targets: emit writes every target's per-target blend switch, target 0's too, and assembleRecord keeps
// the registers of the colours the key names, a target it does not name left out (record_parts.cpp foreign)
#include <gtest/gtest.h>
#include <array>
#include <string>
#include <vector>
#include "../emit.h"
#include "../record_parts.h"

namespace {

constexpr uint32_t kCtWrite = 0x1a00, kBlendPerTarget = 0x1e00;
constexpr char kOneColourKey[] = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0";

uint32_t registerOf(const std::vector<gm20b::EmitRegister> &regs, uint32_t method)
{
    for (const gm20b::EmitRegister &r : regs)
        if (r.method == method)
            return r.value;
    return 0xffffffffu;
}

TEST(RecordTargets, EveryTargetBlendsAlphaByItsOwnFactors)
{
    const std::array<uint8_t, 80> header{};
    gm20b::EmitProgram vertex, fragment;
    vertex.header = header.data();
    fragment.header = header.data();
    std::vector<gm20b::EmitRegister> regs;
    std::string why;
    ASSERT_TRUE(gm20b::emitState("samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0 c1=80:15:0:1:0:0:1:0:0", vertex,
                                 fragment, regs, why))
        << why;
    EXPECT_EQ(registerOf(regs, kBlendPerTarget), 1u);
    EXPECT_EQ(registerOf(regs, kBlendPerTarget + 0x20), 1u);
    EXPECT_NE(gm20b::emittableMethods().count(kBlendPerTarget), 0u);
}

gm20b::CapturedProgram program(const char *id)
{
    gm20b::CapturedProgram p;
    p.id = id;
    p.image.assign(96, 0);
    p.cbufTypes = {gm20b::CbufRoot};
    return p;
}

// The blob of a state of one colour at target 0 whose registers name targets 0, 1 and 2
std::vector<uint32_t> blob()
{
    static const gm20b::CapturedProgram vs = program("vs"), fs = program("fs");
    gm20b::RecordParts parts;
    parts.programs[0] = &vs;
    parts.programs[1] = &fs;
    parts.regs = {{kCtWrite, 0x1111}, {kCtWrite + 4, 0x1000}, {kCtWrite + 8, 0x1111}, {kBlendPerTarget + 0x20, 1}};
    gm20b::PackRules rules;
    rules.varying = {kCtWrite, kCtWrite + 4, kCtWrite + 8, kBlendPerTarget + 0x20};
    gm20b::RuntimeRecord out;
    std::string why;
    EXPECT_TRUE(gm20b::assembleRecord(parts, std::string(kOneColourKey) + " | v vs | f fs", rules, out, why)) << why;
    EXPECT_TRUE(out.extras.empty());
    return out.blob;
}

TEST(RecordTargets, ARecordKeepsOnlyTheRegistersOfTheTargetsItsKeyNames)
{
    EXPECT_EQ(blob(), (std::vector<uint32_t>{kCtWrite, 0x1111}));
}

}  // namespace
