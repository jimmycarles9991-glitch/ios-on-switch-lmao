// SPDX-License-Identifier: MIT
// The context init (gen/SwitchGM20BContext.h) against the one NVK's queue made on the pack generator's mock GM20B
// (testdata/context_capture.txt): the same methods in the same order, every fixed word the same, and every relocated
// word one address that each of its methods took on the mock; less raw's own words, which NVK's init does not write.
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../../SwitchGM20B/SwitchGM20B.h"
#include "../../SwitchGM20B/SwitchGM20BTegra.h"
#include "SwitchGM20BContext.h"
#include "bazel/testing/source_text.h"

namespace {

struct Captured {
    uint32_t subc, mthd, data;
};

std::vector<Captured> capture()
{
    std::istringstream text(switch_ios::test::SourceText("testdata/context_capture.txt"));
    std::vector<Captured> out;
    for (std::string line; std::getline(text, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream fields(line);
        Captured c;
        fields >> c.subc >> std::hex >> c.mthd >> c.data;
        out.push_back(c);
    }
    return out;
}

// Words our init context owns beyond NVK's capture: NVIDIA's CSAA_ENABLE of 0, plus the 8 golden-context registers a
// probe draw may modify and restores from the context (context.tsv, probe_restores.h)
bool gm20bOwn(const switch_gm20b_context_method &m)
{
    if (m.subc != 0)
        return false;
    switch (m.mthd) {
    case SWITCH_NVB197_SET_CSAA_ENABLE:
    case NVB197_SET_CONSTANT_COLOR_RENDERING:
    case NVB197_SET_CONSTANT_COLOR_RENDERING_RED:
    case NVB197_SET_CONSTANT_COLOR_RENDERING_GREEN:
    case NVB197_SET_CONSTANT_COLOR_RENDERING_BLUE:
    case NVB197_SET_CONSTANT_COLOR_RENDERING_ALPHA:
        return m.data == 0;
    case NVB197_SET_SAMPLE_MASK:
        return m.data == 0x1;
    case NVB197_SET_COLOR_TARGET_SAMPLE_MASK:
        return m.data == 0xffff;
    case NVB197_SET_PIPELINE_SHADER(5):
        return m.data == 0x51;
    default:
        return false;
    }
}

std::vector<switch_gm20b_context_method> nvkShared()
{
    std::vector<switch_gm20b_context_method> out;
    for (const switch_gm20b_context_method &m : switch_gm20b_context)
        if (!gm20bOwn(m))
            out.push_back(m);
    return out;
}

TEST(Context, IsTheCapturedInit)
{
    const std::vector<Captured> captured = capture();
    const std::vector<switch_gm20b_context_method> shared = nvkShared();
    ASSERT_EQ(captured.size(), shared.size());
    ASSERT_EQ(shared.size() + 9, SWITCH_GM20B_CONTEXT_METHODS);
    std::map<uint32_t, uint32_t> address;
    for (size_t i = 0; i < captured.size(); i++) {
        const switch_gm20b_context_method &m = shared[i];
        ASSERT_EQ(m.subc, captured[i].subc) << "method " << i;
        ASSERT_EQ(m.mthd, captured[i].mthd) << "method " << i;
        if (m.reloc == SWITCH_GM20B_CONTEXT_FIXED) {
            EXPECT_EQ(m.data, captured[i].data) << "method " << i << " (0x" << std::hex << m.mthd << ")";
            continue;
        }
        EXPECT_EQ(m.data, 0u) << "relocated method " << i;
        const auto [at, fresh] = address.emplace(m.reloc, captured[i].data);
        EXPECT_EQ(at->second, captured[i].data) << "relocation " << m.reloc << " at method " << i;
    }
    EXPECT_EQ(address.size(), size_t(SWITCH_GM20B_CONTEXT_TICKETS_HI));
}

TEST(Context, RelocatesEachAddressWhole)
{
    std::map<uint32_t, int> uses;
    for (const switch_gm20b_context_method &m : switch_gm20b_context)
        uses[m.reloc]++;
    for (const auto [hi, lo] : {std::pair{SWITCH_GM20B_CONTEXT_HEAP_HI, SWITCH_GM20B_CONTEXT_HEAP_LO},
                                {SWITCH_GM20B_CONTEXT_ZERO_HI, SWITCH_GM20B_CONTEXT_ZERO_LO},
                                {SWITCH_GM20B_CONTEXT_ROOT_HI, SWITCH_GM20B_CONTEXT_ROOT_LO},
                                {SWITCH_GM20B_CONTEXT_TIC_HI, SWITCH_GM20B_CONTEXT_TIC_LO},
                                {SWITCH_GM20B_CONTEXT_TSC_HI, SWITCH_GM20B_CONTEXT_TSC_LO},
                                {SWITCH_GM20B_CONTEXT_TICKETS_HI, SWITCH_GM20B_CONTEXT_TICKETS_LO}})
        EXPECT_EQ(uses[hi], uses[lo]) << "relocation " << hi;
}

}  // namespace
