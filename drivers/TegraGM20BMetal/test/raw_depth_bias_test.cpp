// SPDX-License-Identifier: MIT
// raw_depth_bias.h against the depths an M4 Max stores (testdata/nonfinite_depth_bias.tsv): the factors the raw encoder
// writes, biased as GM20B's polygon offset is documented to (Vulkan's depth bias: constant x r + slope x m, then
// bounded by a clamp of either sign; r 2^-24 under SET_DEPTH_BIAS_CONTROL's FORCE_UNORM, else a float zeta's 2^(e-23)
// and a Depth16Unorm pair's code) and clamped by the pass's pixel clamp, give Metal's depth on every row but one kind:
// a Depth32Float flat triangle at window depth 0 under an infinite or NaN constant, where Apple's unit is 0 and the
// bias NaN.
#include "drivers/TegraGM20BMetal/raw/runtime/raw_depth_bias.h"
#include "bazel/testing/source_text.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Row {
    bool d16;
    double lo, hi, z, m;
    float constant, slope, clamp;
    double metal;
    std::string text;
};

std::vector<Row> rows()
{
    std::istringstream in(switch_ios::test::SourceText("testdata/nonfinite_depth_bias.tsv"));
    std::vector<Row> out;
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream f(line);
        std::string format, c, k, cl;
        Row r;
        f >> format >> r.lo >> r.hi >> r.z >> r.m >> c >> k >> cl >> r.metal;
        r.d16 = format == "d16";
        r.constant = std::strtof(c.c_str(), nullptr);
        r.slope = std::strtof(k.c_str(), nullptr);
        r.clamp = std::strtof(cl.c_str(), nullptr);
        r.text = line;
        out.push_back(r);
    }
    return out;
}

double unit(const raw::DepthBiasFactors &f, const Row &r)
{
    if (f.unorm24)
        return std::ldexp(1.0, -24);
    if (r.d16)
        return 1.0 / 65535.0;
    int e = 0;
    std::frexp(r.z, &e);
    return r.z == 0 ? 0.0 : std::ldexp(1.0, e - 1 - 23);
}

double stored(const Row &r)
{
    const raw::DepthBiasFactors f = raw::depthBiasFactors(r.constant, r.slope, r.clamp);
    double o = double(f.constant) * unit(f, r) + double(f.slope) * r.m;
    if (f.clamp > 0)
        o = std::min(o, double(f.clamp));
    else if (f.clamp < 0)
        o = std::max(o, double(f.clamp));
    const double depth = std::clamp(r.z + o, r.lo, r.hi);
    return r.d16 ? std::round(depth * 65535.0) / 65535.0 : depth;
}

bool appleUnitZero(const Row &r)
{
    return !r.d16 && r.z == 0 && r.m == 0 && !std::isfinite(r.constant);
}

TEST(DepthBias, NonFiniteFactorsStoreTheM4MaxsDepth)
{
    const std::vector<Row> table = rows();
    ASSERT_GT(table.size(), 300u);
    size_t matched = 0, zeroUnit = 0;
    for (const Row &r : table) {
        const double got = stored(r);
        if (std::abs(got - r.metal) <= (r.d16 ? 1.5 / 65535.0 : 1e-6)) {
            matched++;
            continue;
        }
        EXPECT_TRUE(appleUnitZero(r)) << r.text << ": GM20B stores " << got;
        zeroUnit++;
    }
    EXPECT_EQ(matched + zeroUnit, table.size());
    EXPECT_EQ(zeroUnit, 8u) << "the Depth32Float flat triangles at depth 0 that Apple biases by NaN";
}

TEST(DepthBias, FiniteFactorsKeepTheirUnitsAndANonFiniteClampBoundsNothing)
{
    for (const float clamp : {INFINITY, -INFINITY, NAN}) {
        const raw::DepthBiasFactors f = raw::depthBiasFactors(100, 2, clamp);
        EXPECT_EQ(f.constant, 100);
        EXPECT_EQ(f.slope, 2);
        EXPECT_EQ(f.clamp, 0);
        EXPECT_FALSE(f.unorm24);
    }
}

TEST(DepthBias, TheSlopeTermOutweighsTheConstantOnEverySlopeWithoutOverflow)
{
    const double saturating = double(raw::kSaturatingBias) * std::ldexp(1.0, -24);
    for (const int e : {-83, -40, 0, 8, 27}) {
        const double m = std::ldexp(1.0, e);
        const float term = raw::kSlopeOutweighs * float(m);
        EXPECT_TRUE(std::isfinite(term)) << e;
        EXPECT_GE(double(term), 2 * saturating) << e;
    }
}

}  // namespace
