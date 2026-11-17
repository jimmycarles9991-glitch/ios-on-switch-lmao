// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_mip.h"
#include "../raw/runtime/raw_record.h"
#include "../raw/runtime/raw_samples.h"
#include "../raw/runtime/raw_turn_copy.h"
#include <gtest/gtest.h>
#include <set>

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
             {"ycbcr_vert  | f ycbcr_load_plane_packed_r_frag", "raw.ycbcr.ycbcr_load_plane_packed_r_frag"},
             {"ycbcr_vert  | f ycbcr_load_plane_packed_rg_frag", "raw.ycbcr.ycbcr_load_plane_packed_rg_frag"},
             {"ycbcr_vert  | f ycbcr_load_srgb_frag", "raw.ycbcr.ycbcr_load_srgb_frag"},
             {"ycbcr_vert  | f ycbcr_load_packed_codes10_frag", "raw.ycbcr.ycbcr_load_packed_codes10_frag"},
             {"ycbcr_store_vert  | f ycbcr_store_chroma16_srgb_masked_frag",
              "raw.ycbcr_store.ycbcr_store_chroma16_srgb_masked_frag"},
             {"ycbcr_store_vert  | f ycbcr_store_luma_packed_masked_frag",
              "raw.ycbcr_store.ycbcr_store_luma_packed_masked_frag"},
             {"ycbcr_store_vert  | f ycbcr_store_plane_packed_frag", "raw.ycbcr_store.ycbcr_store_plane_packed_frag"},
             {"xr_vert  | f xr_bgra10_frag", "raw.xr.xr_bgra10_frag"},
             {"xr_vert  | f xr_planes_merge_frag", "raw.xr.xr_planes_merge_frag"},
             {"xr_vert  | f xr_planes_split_frag", "raw.xr.xr_planes_split_frag"},
             {"xr_vert  | f xr_tile_bgra10_frag", "raw.xr.xr_tile_bgra10_frag"},
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
    EXPECT_EQ(states, 21);
}

// Every format raw_mip.h draws a 3D texture's planes for finds its pack state under the key the probe gives it
TEST(RawKey, EveryVolumeMipStateIsAnOwnState)
{
    int states = 0;
    for (uint32_t format = 0; format < 700; format++) {
        const char *name = raw::mip3dState(format);
        if (!name)
            continue;
        states++;
        const std::string fragment = std::string(name).substr(sizeof "raw.mip." - 1);
        const std::string key =
            "samples=1 topo=0 ds=0/0 c0=" + std::to_string(format) + ":15:0:1:0:0:1:0:0  | v mip_vert  | f " + fragment;
        EXPECT_EQ(raw::packKeyHash(key), hash(name)) << format;
        EXPECT_NE(raw::mipState(format), nullptr) << format;
    }
    EXPECT_EQ(states, 21);
}

// Every depth conversion raw_turn_copy.h names, a multisampled one's at each sample count, finds its pack state
TEST(RawKey, EveryDepthConversionStateIsAnOwnState)
{
    int states = 0;
    for (uint32_t format : {250u, 252u, 260u})
        for (uint32_t samples : {1u, 2u, 4u}) {
            const char *name = raw::turnDepthState(format, samples);
            if (!name)
                continue;
            states++;
            const std::string fragment = std::string(name).substr(sizeof "raw.turn." - 1);
            const std::string key = "samples=" + std::to_string(samples) + " topo=0 ds=" + std::to_string(format) +
                                    "/0  | v turn_vert  | f " + fragment;
            EXPECT_EQ(raw::packKeyHash(key), hash(name)) << format << " x" << samples;
        }
    EXPECT_EQ(states, 7);
}

// Every colour resolve raw_samples.h names finds its pack state under the key the probe gives it
TEST(RawKey, EveryResolveStateIsAnOwnState)
{
    std::set<std::string> names;
    for (uint32_t format = 0; format < 700; format++)
        for (uint32_t samples : {2u, 4u}) {
            const char *name = raw::resolveState(format, samples);
            if (!name)
                continue;
            names.insert(name);
            const std::string fragment = std::string(name).substr(sizeof "raw.resolve." - 1);
            const std::string key = "samples=1 topo=0 ds=0/0 c0=" + std::to_string(format) +
                                    ":15:0:1:0:0:1:0:0  | v resolve_vert  | f " + fragment;
            EXPECT_EQ(raw::packKeyHash(key), hash(name)) << format << " x" << samples;
        }
    EXPECT_EQ(names.size(), 23u);
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
