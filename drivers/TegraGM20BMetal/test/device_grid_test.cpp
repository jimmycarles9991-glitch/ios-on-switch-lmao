// SPDX-License-Identifier: MIT
// A dispatch past the QMD's grid fields (2^31 - 1 threadgroups across, 65535 down and deep) on gm20b::Device: launched in
// slices within them, each QMD's raster its slice and its root table's cs.base_group where the slice starts with
// cs.group_count the whole grid, read off the device's submitted words and the memory they point at; an indirect
// dispatch command's region written as one launch per slice.
#include "device_harness.h"
#include "../compute.h"
#include "../icb_compute.h"
#include <vector>

namespace {

constexpr uint32_t kQmdRootLo = 29;
constexpr uint32_t kQmdCbufValid = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CONSTANT_BUFFER_VALID(0));
constexpr uint32_t kQmdRasterWidth = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CTA_RASTER_WIDTH),
                   kQmdRasterHeight = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CTA_RASTER_HEIGHT);

const gm20b::ComputeRecord &rootKernel()
{
    static const gm20b::ComputeRecord r = [] {
        gm20b::ComputeRecord k;
        k.code.assign(64, 0);
        k.cbufType[0] = gm20b::CbufRoot;
        k.cbufs = 1;
        k.kernel.regionWord = -1;
        k.kernel.qmd[kQmdCbufValid] |= 1;
        return k;
    }();
    return r;
}

uint32_t word(uint64_t va)
{
    for (const auto &[at, bound] : kernel_model::model.binds) {
        const auto bytes = kernel_model::model.memory.find(uint32_t(bound.first));
        if (va < at || va + 4 > at + bound.second || bytes == kernel_model::model.memory.end() ||
            va - at + 4 > bytes->second.size())
            continue;
        uint32_t w;
        memcpy(&w, bytes->second.data() + (va - at), 4);
        return w;
    }
    return 0xdeadbeef;
}

struct Launch {
    uint32_t width, height, depth;
    uint32_t base[3], groups[3];
};

class DeviceGrid : public harness::DeviceTest {
protected:
    bool dispatched(const uint32_t groups[3], std::vector<Launch> &out, const char **why, bool measured = true)
    {
        const gm20b::ComputeState *k = gm20b::placeCompute(dev, rootKernel());
        if (k) {
            auto &rate = const_cast<gm20b::BandRate &>(k->rate);
            rate.psPerUnit.store(0, std::memory_order_relaxed);
            rate.samples.store(0, std::memory_order_relaxed);
            if (measured)
                gm20b::learn(rate, 40'000, 1'000'000);
        }
        gm20b::CommandBuffer *cb = dev.begin();
        if (!k || !cb)
            return false;
        const size_t before = kernel_model::model.jobs.size();
        bool ok;
        {
            gm20b::ComputeEncoder e(*cb, dev);
            e.setState(k);
            const uint32_t threads[3] = {1, 1, 1};
            ok = e.dispatch(groups, threads, why);
            e.end();
        }
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        for (size_t j = before; j < kernel_model::model.jobs.size(); j++) {
            const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
            for (size_t i = 0; i + 1 < w.size(); i++) {
                const uint32_t h = w[i], op = h >> 29, subc = h >> 13 & 7, m = (h & 0xfff) << 2;
                if (subc != gm20b::SUBC_COMPUTE || m != NVB1C0_SEND_PCAS_A)
                    continue;
                const uint64_t qmd = uint64_t(op == 4 ? h >> 16 & 0x1fff : w[i + 1]) << 8;
                const uint64_t root =
                    uint64_t(word(qmd + 4 * (kQmdRootLo + 1)) & 0xff) << 32 | word(qmd + 4 * kQmdRootLo);
                const uint32_t hd = word(qmd + 4 * kQmdRasterHeight);
                out.push_back({word(qmd + 4 * kQmdRasterWidth),
                               hd & 0xffff,
                               hd >> 16,
                               {word(root), word(root + 4), word(root + 8)},
                               {word(root + 12), word(root + 16), word(root + 20)}});
            }
        }
        return ok;
    }
};

TEST_F(DeviceGrid, AGridTallerThanTheQmdIsLaunchedInSlicesOfIt)
{
    const uint32_t groups[3] = {3, 70000, 1};
    std::vector<Launch> launches;
    const char *why = nullptr;
    ASSERT_TRUE(dispatched(groups, launches, &why)) << (why ? why : "");
    ASSERT_EQ(launches.size(), 2u);
    EXPECT_EQ(launches[0].width, 3u);
    EXPECT_EQ(launches[0].height, 65535u);
    EXPECT_EQ(launches[0].base[1], 0u);
    EXPECT_EQ(launches[1].height, 70000u - 65535u);
    EXPECT_EQ(launches[1].base[1], 65535u);
    for (const Launch &l : launches) {
        EXPECT_EQ(l.depth, 1u);
        EXPECT_EQ(l.base[0], 0u);
        EXPECT_EQ(l.base[2], 0u);
        EXPECT_EQ(l.groups[0], 3u);
        EXPECT_EQ(l.groups[1], 70000u) << "cs.group_count is the whole grid's";
        EXPECT_EQ(l.groups[2], 1u);
    }
}

TEST_F(DeviceGrid, AGridDeeperAndTallerThanTheQmdTakesASliceOfEach)
{
    const uint32_t groups[3] = {1, 65536, 65536 + 9};
    std::vector<Launch> launches;
    const char *why = nullptr;
    ASSERT_TRUE(dispatched(groups, launches, &why)) << (why ? why : "");
    // a dispatch this long is also cut in time bands (dispatch_bands.h): at least the four slices
    ASSERT_GE(launches.size(), 4u);
    uint64_t covered = 0;
    for (const Launch &l : launches) {
        EXPECT_LE(l.height, 0xffffu);
        EXPECT_LE(l.depth, 0xffffu);
        EXPECT_LE(uint64_t(l.base[1]) + l.height, 65536u);
        EXPECT_LE(uint64_t(l.base[2]) + l.depth, 65536u + 9);
        covered += uint64_t(l.width) * l.height * l.depth;
    }
    EXPECT_EQ(covered, uint64_t(65536) * (65536 + 9));
}

TEST_F(DeviceGrid, AGridWithinTheQmdIsOneLaunch)
{
    const uint32_t groups[3] = {4, 65535, 2};
    std::vector<Launch> launches;
    const char *why = nullptr;
    ASSERT_TRUE(dispatched(groups, launches, &why)) << (why ? why : "");
    ASSERT_EQ(launches.size(), 1u);
    EXPECT_EQ(launches[0].height, 65535u);
    EXPECT_EQ(launches[0].depth, 2u);
}

TEST_F(DeviceGrid, SmallUnmeasuredGridStaysInQmdSlicesWhileHeavyUnmeasuredCapsAtProbeLimit)
{
    const uint32_t groups[3] = {3, 70000, 1};
    std::vector<Launch> launches;
    const char *why = nullptr;
    ASSERT_TRUE(dispatched(groups, launches, &why, false)) << (why ? why : "");
    EXPECT_EQ(launches.size(), 2u);

    gm20b::ComputeRecord heavy = rootKernel();
    heavy.code.assign(4096 * 8, 0);
    const gm20b::ComputeState *hk = gm20b::placeCompute(dev, heavy);
    ASSERT_NE(hk, nullptr);
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t before = kernel_model::model.jobs.size();
    {
        gm20b::ComputeEncoder e(*cb, dev);
        e.setState(hk);
        const uint32_t bigGroups[3] = {16, 70000, 1};
        const uint32_t threads[3] = {1, 1, 1};
        ASSERT_TRUE(e.dispatch(bigGroups, threads, &why)) << (why ? why : "");
        e.end();
    }
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    size_t heavyLaunches = 0;
    for (size_t j = before; j < kernel_model::model.jobs.size(); j++) {
        const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
        for (size_t i = 0; i + 1 < w.size(); i++) {
            const uint32_t h = w[i], subc = h >> 13 & 7, m = (h & 0xfff) << 2;
            if (subc == gm20b::SUBC_COMPUTE && m == NVB1C0_SEND_PCAS_A)
                heavyLaunches++;
        }
    }
    EXPECT_EQ(heavyLaunches, gm20b::kMaxUnmeasuredBands);
}

TEST_F(DeviceGrid, AnIndirectCommandsSliceIsItsOwnLaunch)
{
    const gm20b::ComputeState *k = gm20b::placeCompute(dev, rootKernel());
    ASSERT_NE(k, nullptr);
    std::vector<uint8_t> block(gm20b::icb::computeRegionBytes(0));
    gm20b::ComputeRegion region;
    region.local = {1, 1, 1};
    region.groups = {2, 70000, 1};
    gm20b::ComputeEncoder::IcbBuffer buffers[31] = {};
    const char *why = nullptr;
    EXPECT_FALSE(gm20b::ComputeEncoder::icbRegion(dev, *k, region, {2, 70000, 1}, {1, 1, 1}, buffers, gm20b::StageIn{},
                                                  block.data(), 0x123400, &why))
        << "one QMD cannot hold 70000 threadgroups down";
    const gm20b::DispatchPiece second = gm20b::qmdSlice({{}, region.groups}, 1);
    ASSERT_TRUE(gm20b::ComputeEncoder::icbRegion(dev, *k, region, second, {2, 70000, 1}, {1, 1, 1}, buffers,
                                                 gm20b::StageIn{}, block.data(), 0x123400, &why))
        << (why ? why : "");
    const auto *qmd = reinterpret_cast<const uint32_t *>(block.data());
    const auto *root = qmd + gm20b::icb::kComputeQmdBytes / 4;
    EXPECT_EQ(qmd[kQmdRasterWidth], 2u);
    EXPECT_EQ(qmd[kQmdRasterHeight] & 0xffff, 70000u - 65535u);
    EXPECT_EQ(root[1], 65535u);
    EXPECT_EQ(root[4], 70000u);
}

}  // namespace
