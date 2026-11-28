// SPDX-License-Identifier: MIT
// A state's written colour targets (Pipeline::writeBits) over a pack whose baseline leaves SET_CT_WRITE of targets 6 and 7
// enabled, as the 24B119 pack's does (blend_kill_pack_test.py): only the targets the state's key names count, so a
// one-target state writes target 0 alone and a depth-only one writes none, while its SET_CT_WRITE words stay the
// hardware's (Pipeline::ctWrite).
#include "device_harness.h"
#include "../mixed_samples.h"
#include "../record.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kCtWrite = 0x1a00, kAllChannels = 0x1111;

std::string packWithBaselineWrites()
{
    const char *dir = getenv("TEST_TMPDIR");
    const std::string path = std::string(dir ? dir : "/tmp") + "/baseline-writes-pack.bin";
    gm20b::PackHeader header{};
    memcpy(header.magic, "GM20BPK1", 8);
    header.version = gm20b::kPackVersion;
    const uint32_t baseline[] = {kCtWrite + 4 * 6, kAllChannels, kCtWrite + 4 * 7, kAllChannels};
    const uint64_t baseOff = sizeof(header);
    for (auto &s : header.section)
        s.offset = baseOff;
    header.section[gm20b::PackBaseline] = {baseOff, sizeof(baseline)};
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return {};
    fwrite(&header, 1, sizeof(header), f);
    fwrite(baseline, 1, sizeof(baseline), f);
    fclose(f);
    return path;
}

class DeviceWriteBits : public testing::Test {
protected:
    gm20b::Device dev;

    void SetUp() override
    {
        kernel_model::model = {};
        const std::string pack = packWithBaselineWrites();
        ASSERT_FALSE(pack.empty());
        std::string err;
        ASSERT_TRUE(dev.open(pack.c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    }

    gm20b::Pipeline *state(const char *keyString, std::vector<uint32_t> blob)
    {
        gm20b::RuntimeRecord r;
        r.keyString = keyString;
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.programs[0].id = "vs";
        r.programs[1].id = std::string("fs") + keyString;
        r.pipeline.keyHash = gm20b::Pack::fnv(keyString, strlen(keyString));
        r.blob = std::move(blob);
        std::string why;
        gm20b::Pipeline *pl = dev.addRecord(r, false, why);
        EXPECT_NE(pl, nullptr) << why;
        return pl;
    }
};

TEST_F(DeviceWriteBits, OnlyTheTargetsAStatesKeyNamesAreWritten)
{
    const gm20b::Pipeline *one =
        state("samples=1 topo=3 ds=0/0 c0=80:15:1:1:5:0:1:5:0 | v vs | f fs", {kCtWrite, kAllChannels});
    ASSERT_NE(one, nullptr);
    EXPECT_EQ(one->writeBits, 0x01);
    EXPECT_EQ(one->ctWrite[6], kAllChannels) << "the hardware's SET_CT_WRITE stays the baseline's";
    const gm20b::Pipeline *depth = state("samples=1 topo=3 ds=252/0 | v vs | f fs", {});
    ASSERT_NE(depth, nullptr);
    EXPECT_EQ(depth->writeBits, 0x00);
    const gm20b::Pipeline *seventh =
        state("samples=1 topo=3 ds=0/0 c0=80:15:1:1:5:0:1:5:0 c6=80:15:1:1:5:0:1:5:0 | v vs | f fs",
              {kCtWrite, kAllChannels});
    ASSERT_NE(seventh, nullptr);
    EXPECT_EQ(seventh->writeBits, 0x41);
}

// A colourless four-sample state beside a one-sample colour target (mixed_samples.h): it writes no colour, so it is drawn
// whole, never split into a second fragment run over samples 1-3
TEST_F(DeviceWriteBits, AColourlessFourSampleStateInAMixedPassIsDrawnWhole)
{
    const gm20b::Pipeline *depth = state("samples=4 topo=3 ds=252/0 | v vs | f fs", {});
    ASSERT_NE(depth, nullptr);
    ASSERT_EQ(depth->samples, 4);
    gm20b::Texture zeta;
    zeta.format = 252;  // Depth32Float
    gm20b::DepthStencilAttachment zs;
    zs.texture = &zeta;
    gm20b::DepthStencilDesc ds;
    ds.depthCompare = 1;  // Less
    ds.depthWrite = true;
    EXPECT_EQ(gm20b::mixedDraw(*depth, ds, zs, 1, 4, false), gm20b::MixedDraw::Whole);
}

// A pass of eight cleared targets whose first draw covers the area with Core Animation's transform state, which names
// target 0 alone: the draw's coverage elides target 0's clear and no other, as Metal never writes targets 6 and 7 for
// the state whatever the baseline's SET_CT_WRITE (pass_clears.cpp covered_)
TEST_F(DeviceWriteBits, ACoveringDrawElidesOnlyTheClearsOfTheTargetsItsStateNames)
{
    gm20b::RuntimeRecord r;
    r.keyString = "samples=1 c0=70:15:0 ds=0/0 a0=31:0:0:16:1:1 ";
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.programs[0].id = "vs";
    r.programs[1].id = "fs-transform";
    r.pipeline.keyHash = 0x9e02;
    r.pipeline.vertexSlot = 0;
    r.pipeline.vertexStride = 16;
    r.blob = {kCtWrite, kAllChannels, 0x1c00, 1u << 12 | 16};  // SET_VERTEX_STREAM_A_FORMAT(0): enabled, 16 bytes
    std::string why;
    gm20b::Pipeline *pl = dev.addRecord(r, true, why);
    ASSERT_NE(pl, nullptr) << why;
    ASSERT_TRUE(pl->caTransform && pl->positionComponents == 4);
    EXPECT_EQ(pl->writeBits, 0x01);
    gm20b::Texture targets[8];
    gm20b::PassDesc desc;
    for (uint32_t j = 0; j < 8; j++) {
        ASSERT_TRUE(dev.createTexture(64, 64, 70, targets[j]));
        desc.color[j].texture = &targets[j];
        desc.color[j].load = gm20b::ColorAttachment::Clear;
        desc.color[j].store = true;
    }
    desc.count = 8;
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const uint64_t clears = dev.stats.clears.load(), elided = dev.stats.clearsElided.load();
    gm20b::RenderEncoder *e = cb->render(desc);
    ASSERT_NE(e, nullptr);
    e->setPipeline(pl);
    const float corners[16] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1};
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    e->setBytes(1, 0, corners, sizeof corners);
    e->setBytes(1, 2, identity, sizeof identity);
    const char *refused = nullptr;
    ASSERT_TRUE(e->draw(4, 0, 4, 1, 0, &refused)) << (refused ? refused : "");
    e->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    EXPECT_EQ(dev.stats.clears.load() - clears, 8u);
    EXPECT_EQ(dev.stats.clearsElided.load() - elided, 1u) << "target 0's alone";
}

}  // namespace
