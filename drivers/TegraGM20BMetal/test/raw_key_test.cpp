// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_mip.h"
#include "../raw/runtime/raw_record.h"
#include <gtest/gtest.h>

namespace {

uint64_t hash(const std::string &s)
{
    return raw::Pack::fnv(s.data(), s.size());
}

TEST(RawKey, InternalLookupsFindCanonicalOwnStates)
{
    for (const auto &[pair, alias] : std::initializer_list<std::pair<const char *, const char *>>{
             {"ycbcr_vert  | f ycbcr_converted_frag", "raw.ycbcr.ycbcr_converted_frag"},
             {"ycbcr_vert  | f ycbcr_packed_plane_frag", "raw.ycbcr.ycbcr_packed_plane_frag"},
             {"ycbcr_store_vert  | f ycbcr_store_plane_packed_frag", "raw.ycbcr_store.ycbcr_store_plane_packed_frag"},
             {"xr_vert  | f xr_bgra10_frag", "raw.xr.xr_bgra10_frag"},
             {"two_plane_cover  | f two_plane_load", "raw.two_plane.two_plane_load"},
             {"resolve_vert  | f resolve_bgra8_frag", "raw.resolve.resolve_bgra8_frag"}}) {
        const std::string key = std::string("samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v ") + pair;
        EXPECT_EQ(raw::packKeyHash(key), hash(alias));
        for (const char *suffix : {" | l RenderBox", " | air 1234", " | cubic 1"})
            EXPECT_EQ(raw::packKeyHash(key + suffix), hash(key + suffix));
    }
}

// Every format raw_mip.h draws a chain for finds its pack state under the key the probe gives it
TEST(RawKey, EveryMipStateIsAnOwnState)
{
    int states = 0;
    for (uint32_t format = 0; format < 700; format++) {
        const char *name = raw::mipState(format);
        if (!name)
            continue;
        states++;
        const std::string fragment = std::string(name).substr(sizeof "raw.mip." - 1);
        const std::string key =
            "samples=1 topo=0 ds=0/0 c0=" + std::to_string(format) + ":15:0:1:0:0:1:0:0  | v mip_vert  | f " + fragment;
        EXPECT_EQ(raw::packKeyHash(key), hash(name)) << format;
    }
    EXPECT_EQ(states, 16);
}

TEST(RawKey, FrameworkResolveSpecializationsHaveDistinctKeys)
{
    const std::string a = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v plane__vertex  | f "
                          "resolve_sample_fragment #0:36:01";
    const std::string b = a.substr(0, a.size() - 1) + "2";
    EXPECT_EQ(raw::packKeyHash(a), hash(a));
    EXPECT_EQ(raw::packKeyHash(b), hash(b));
    EXPECT_NE(raw::packKeyHash(a), raw::packKeyHash(b));
}

}  // namespace
