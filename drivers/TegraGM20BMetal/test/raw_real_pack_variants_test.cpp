// SPDX-License-Identifier: MIT
// A device build's pack (SWITCH_REAL_PACK) against the names the client asks it by: every variant record the pack builder
// filed (raw/pack/directpack.mm's names: cubic, test class, two-plane colours, sampled two-plane slots, written formats)
// is named exactly as RawMetalCubic.mm names the draw's variant (raw_render_variant.h RenderVariant::name()), so a draw
// finds every variant the pack holds; and the amplified and tessellation states it holds are counted. Skipped where no
// pack is given.
#include "../raw/runtime/raw_pack.h"
#include "../raw/runtime/raw_render_variant.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>

namespace {

// " | cubic s<slot>:<s><t><r><border><n|p>... t<slot>:<format>..." (raw_cubic.h CubicBinds::key)
bool cubicOf(const std::string &segment, raw::CubicBinds &out)
{
    std::istringstream words(segment.substr(5));
    for (std::string w; words >> w;) {
        const size_t colon = w.find(':');
        if (colon == std::string::npos || colon < 2)
            return false;
        const uint8_t slot = uint8_t(atoi(w.c_str() + 1));
        const std::string value = w.substr(colon + 1);
        if (w[0] == 's' && value.size() == 5 && (value[4] == 'n' || value[4] == 'p'))
            out.samplers.push_back({slot, uint8_t(value[0] - '0'), uint8_t(value[1] - '0'), uint8_t(value[2] - '0'),
                                    uint8_t(value[3] - '0'), value[4] == 'n'});
        else if (w[0] == 't')
            out.textures.push_back({slot, uint32_t(strtoul(value.c_str(), nullptr, 10))});
        else
            return false;
    }
    return !out.empty();
}

// The variant a record's name names, segment by segment; false for a segment no RenderVariant field holds
bool variantOf(const std::string &name, raw::RenderVariant &v, std::map<std::string, unsigned> &forms)
{
    for (size_t at = name.find(" | "); at != std::string::npos;) {
        const size_t next = name.find(" | ", at + 3);
        const std::string segment = name.substr(at + 3, (next == std::string::npos ? name.size() : next) - at - 3);
        if (segment.rfind("cubic ", 0) == 0 && cubicOf(segment, v.cubic))
            forms["cubic"]++;
        else if (segment.rfind("tests=", 0) == 0 && (v.fragmentTests = uint8_t(atoi(segment.c_str() + 6))))
            forms["tests"]++;
        else if (segment == "xr2p" && (v.twoPlane = true))
            forms["xr2p"]++;
        else if (segment.rfind("xrs=", 0) == 0 && (v.xrSlots = uint16_t(strtoul(segment.c_str() + 4, nullptr, 10))))
            forms["xrs"]++;
        else if (raw::parseWritten(segment, v.written))
            forms["wtex"]++;
        else
            return false;
        at = next;
    }
    return true;
}

TEST(RealPackVariants, EveryVariantTheClientAsksForByTheNameThePackFiledItUnder)
{
    const char *path = getenv("SWITCH_REAL_PACK");
    if (!path)
        GTEST_SKIP() << "no SWITCH_REAL_PACK";
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(path, 0, err)) << err;
    const raw::PackPipeline *records = pack.section<raw::PackPipeline>(raw::PackPipelines);
    const size_t count = pack.bytes(raw::PackPipelines) / sizeof(raw::PackPipeline);
    std::map<std::string, unsigned> forms;
    unsigned named = 0, misnamed = 0, amplified = 0, tessellated = 0;
    for (size_t i = 0; i < count; i++) {
        const raw::PackPipeline &r = records[i];
        const std::string fields = pack.string(r.keyString);
        amplified += fields.find(" amp=") != std::string::npos;
        tessellated += fields.find(" t=") != std::string::npos;
        if (!r.variantName)
            continue;
        const std::string name = pack.string(r.variantName);
        raw::RenderVariant v;
        named++;
        if (!variantOf(name, v, forms) || v.name() != name) {
            misnamed++;
            ADD_FAILURE() << "a variant the client names \"" << v.name() << "\" filed as \"" << name << "\" under "
                          << std::hex << r.keyHash;
        }
    }
    printf("pack %s: %zu records, %u variant records, %u misnamed, %u amplified, %u tessellation\n", path, count, named,
           misnamed, amplified, tessellated);
    for (const auto &[form, n] : forms)
        printf("  %s %u\n", form.c_str(), n);
    EXPECT_GT(named, 0u);
}

}  // namespace
