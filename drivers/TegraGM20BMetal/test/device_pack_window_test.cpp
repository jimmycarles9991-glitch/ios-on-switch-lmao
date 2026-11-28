// SPDX-License-Identifier: MIT
#include "kernel_model.h"
#include "../gpu.h"
#include "../pack.h"
#include "../program_pages.h"

#include <gtest/gtest.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t kPackStack = 0x123456789abcdef0ull;
constexpr uint64_t kTier0StateA = 0x100, kTier0StateB = 0x200;
constexpr uint64_t kTier1StateA = 0x300, kTier1StateB = 0x400, kTier1StateC = 0x500;
constexpr uint32_t kHeapOffset = uint32_t(gm20b::kPackTierAlign);
constexpr uint32_t kTier0HeapBytes = uint32_t(gm20b::kPackTierAlign);
constexpr uint32_t kTier1HeapBytes = uint32_t(2 * gm20b::kProgramGrain);
constexpr uint32_t kTotalHeapBytes = kTier0HeapBytes + kTier1HeapBytes;
constexpr uint32_t kSetProgramRegionA = 0x1608, kSetProgramRegionB = 0x160c;
constexpr uint32_t kSetPipelineProgramVs = 0x2044, kSetPipelineProgramFs = 0x2144;
constexpr uint32_t kCbSelectorA = 0x2380;

struct BuiltPack {
    std::string path;
};

BuiltPack writeTwoTierPack(const char *name, uint32_t tier1SlmLow = 0)
{
    constexpr uint32_t kInit = 0x1000;
    constexpr uint32_t kPrograms = 0x4000;
    constexpr uint32_t kPipelines = 0x8000;
    constexpr uint32_t kStrings = 0xc000;
    constexpr uint32_t kBlob = 0x10000;
    constexpr uint32_t kFastKeys = 0x14000;

    std::vector<uint8_t> file(kHeapOffset + kTotalHeapBytes, 0);
    gm20b::PackHeader h = {};
    std::memcpy(h.magic, "GM20BPK1", 8);
    h.version = gm20b::kPackVersion;
    h.programs = 7;
    h.pipelines = 5;
    h.stack = kPackStack;

    const gm20b::PackInitMethod init[2] = {
        {0, kSetProgramRegionA, 0, gm20b::RelocHeapHi},
        {0, kSetProgramRegionB, 0, gm20b::RelocHeapLo},
    };

    gm20b::PackProgram programs[7] = {};
    const uint32_t offsets[7] = {
        0x0,
        0x1000,
        0x4000,
        kTier0HeapBytes + 0x0,
        kTier0HeapBytes + 0x1000,
        kTier0HeapBytes + 0x4000,
        kTier0HeapBytes + uint32_t(gm20b::kProgramGrain),
    };
    const uint32_t sizes[7] = {0x200, 0x2000, 0x300, 0x200, 0x2000, 0x200, 0x2000};
    for (uint32_t i = 0; i < 7; ++i) {
        programs[i].heapOffset = offsets[i];
        programs[i].program = offsets[i] + 0x30;
        programs[i].bytes = sizes[i];
        programs[i].gprs = 8;
        programs[i].cbufs = 1;
        programs[i].cbufType[0] = gm20b::CbufRoot;
        if (i == 0 || i == 1 || i == 4) {
            programs[i].dataOffset = i == 0 ? 0x100 : 0x1000;
            programs[i].dataBytes = 0x100;
            programs[i].cbufs = 2;
            programs[i].cbufType[1] = gm20b::CbufShaderData;
        }
        for (uint32_t b = 0x80; b < sizes[i]; ++b)
            file[kHeapOffset + offsets[i] + b] = uint8_t(b * 7 + i);
        if (tier1SlmLow && i == 3)
            std::memcpy(file.data() + kHeapOffset + programs[i].program + 4, &tier1SlmLow, sizeof tier1SlmLow);
    }

    gm20b::PackPipeline pp[5] = {};
    const uint64_t keys[5] = {kTier0StateA, kTier0StateB, kTier1StateA, kTier1StateB, kTier1StateC};
    const uint32_t vs[5] = {0, 0, 3, 3, 3};
    const uint32_t fs[5] = {1, 2, 4, 5, 6};
    for (uint32_t i = 0; i < 5; ++i) {
        pp[i].keyHash = keys[i];
        pp[i].keyString = 1;
        pp[i].vs = vs[i];
        pp[i].fs = fs[i];
        pp[i].blobDwords = 2;
        for (gm20b::PackUniformRange &u : pp[i].uniform)
            u.entry = gm20b::kNoUniform;
    }

    gm20b::PackFastKey fastKeys[5] = {};
    for (uint32_t i = 0; i < 5; ++i)
        fastKeys[i] = {keys[i], i, 1};

    const char strings[] = "\0samples=1 c0=70:15:0 ds=0/0 ";
    const uint32_t blob[] = {0x1a00, 0x1111};
    const auto put = [&](gm20b::PackSectionId id, uint32_t at, const void *bytes, size_t n, uint64_t tier0) {
        h.section[id] = {at, n};
        h.tier0[id] = tier0;
        std::memcpy(file.data() + at, bytes, n);
    };
    put(gm20b::PackInit, kInit, init, sizeof init, sizeof init);
    put(gm20b::PackPrograms, kPrograms, programs, sizeof programs, 3 * sizeof(gm20b::PackProgram));
    put(gm20b::PackPipelines, kPipelines, pp, sizeof pp, 2 * sizeof(gm20b::PackPipeline));
    put(gm20b::PackStrings, kStrings, strings, sizeof strings, sizeof strings);
    put(gm20b::PackBlob, kBlob, blob, sizeof blob, sizeof blob);
    put(gm20b::PackFastKeys, kFastKeys, fastKeys, sizeof fastKeys, 2 * sizeof(gm20b::PackFastKey));
    h.section[gm20b::PackHeap] = {kHeapOffset, kTotalHeapBytes};
    h.tier0[gm20b::PackHeap] = kTier0HeapBytes;
    std::memcpy(file.data(), &h, sizeof h);

    const char *dir = std::getenv("TEST_TMPDIR");
    BuiltPack out;
    out.path = std::string(dir ? dir : "/tmp") + "/" + name + ".bin";
    std::ofstream(out.path, std::ios::binary)
        .write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));
    return out;
}

template <typename Fn> void walkPushMethods(const std::vector<uint32_t> &words, Fn &&fn)
{
    size_t i = 0;
    while (i < words.size()) {
        const uint32_t h = words[i++];
        const uint32_t op = h >> 29;
        const uint32_t count = (h >> 16) & 0x1fff;
        const uint32_t subc = (h >> 13) & 7;
        const uint32_t mthd = (h & 0xfff) << 2;
        if (op == 4) {
            fn(subc, mthd, count);
            continue;
        }
        if (i + count > words.size())
            break;
        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t cur = op == 1 ? mthd + 4 * k : op == 5 ? mthd + (k ? 4u : 0u) : mthd;
            fn(subc, cur, words[i + k]);
        }
        i += count;
    }
}

bool findMethod1(const std::vector<uint32_t> &words, uint32_t mthd, uint32_t &value)
{
    bool found = false;
    walkPushMethods(words, [&](uint32_t subc, uint32_t cur, uint32_t data) {
        if (subc == 0 && cur == mthd) {
            value = data;
            found = true;
        }
    });
    return found;
}

std::vector<uint64_t> findCbSelectorAddresses(const std::vector<uint32_t> &words)
{
    std::vector<uint64_t> out;
    uint32_t hi = 0;
    bool haveHi = false;
    walkPushMethods(words, [&](uint32_t subc, uint32_t cur, uint32_t data) {
        if (subc != 0)
            return;
        if (cur == kCbSelectorA + 4) {
            hi = data;
            haveHi = true;
        } else if (cur == kCbSelectorA + 8 && haveHi) {
            out.push_back((uint64_t(hi) << 32) | data);
            haveHi = false;
        }
    });
    return out;
}

uint64_t programRegionFromJobs(const kernel_model::Model &m)
{
    uint32_t hi = 0, lo = 0;
    for (const kernel_model::Job &job : m.jobs)
        if (findMethod1(job.words, kSetProgramRegionA, hi) && findMethod1(job.words, kSetProgramRegionB, lo))
            return (uint64_t(hi) << 32) | lo;
    return 0;
}

size_t bindsInPackWindow(const kernel_model::Model &m)
{
    size_t n = 0;
    for (const auto &[va, bound] : m.binds)
        if (va < TGM_PACK_WINDOW_VA + TGM_PACK_WINDOW_BYTES && TGM_PACK_WINDOW_VA < va + bound.second)
            ++n;
    return n;
}

void drawState(gm20b::Device &dev, gm20b::Pipeline *pl)
{
    ASSERT_NE(pl, nullptr);
    gm20b::Texture rt;
    ASSERT_TRUE(dev.createTexture(16, 16, 70, rt, false, true));
    gm20b::PassDesc pass;
    pass.count = 1;
    pass.color[0].texture = &rt;
    pass.color[0].load = gm20b::ColorAttachment::Clear;
    pass.color[0].store = true;
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const char *why = nullptr;
    gm20b::RenderEncoder *e = cb->render(pass, &why);
    ASSERT_NE(e, nullptr) << (why ? why : "");
    e->setPipeline(pl);
    ASSERT_TRUE(e->draw(3, 0, 3, 1, 0, &why)) << (why ? why : "");
    e->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    EXPECT_GT(seqno, 0u);
    dev.destroyTexture(rt);
}

void configureReadyPackImage()
{
    kernel_model::model = {};
    kernel_model::model.vaEnd = uint64_t(1) << 39;
    kernel_model::model.packImage.enabled = true;
    kernel_model::model.packImage.status = TegraGM20BPackStatusReady;
    kernel_model::model.packImage.gpuVa = TGM_PACK_WINDOW_VA;
    kernel_model::model.packImage.cpuAddress = 0;
    kernel_model::model.packImage.tier0HeapBytes = kTier0HeapBytes;
    kernel_model::model.packImage.heapBytes = kTotalHeapBytes;
    kernel_model::model.packImage.stack = kPackStack;
    kernel_model::model.packImage.waitedNs = 5000000ull;
}

TEST(GM20BDevicePackWindow, Tier0ExecutesFromSharedWindowWithZeroImportsInRenderServerAndClient)
{
    const BuiltPack pack = writeTwoTierPack("pack-window-tier0");
    configureReadyPackImage();

    gm20b::Device server;
    std::string err;
    ASSERT_TRUE(server.open(pack.path.c_str(), kPackStack, err, gm20b::Device::Role::RenderServer)) << err;
    ASSERT_EQ(kernel_model::model.packImageCalls.size(), 1u);
    EXPECT_EQ(kernel_model::model.packImageCalls[0].kr, KERN_SUCCESS);
    EXPECT_EQ(kernel_model::model.packImageCalls[0].status, TegraGM20BPackStatusReady);
    EXPECT_EQ(server.packWindowCpu(), nullptr);
    EXPECT_EQ(server.heapBytes(), 0u);
    EXPECT_EQ(server.heapInPlaceBytes(), 0u);
    EXPECT_TRUE(kernel_model::model.imports.empty());
    EXPECT_EQ(bindsInPackWindow(kernel_model::model), 0u);
    EXPECT_EQ(programRegionFromJobs(kernel_model::model), TGM_PACK_WINDOW_VA);

    gm20b::Pipeline *a = server.pipeline(kTier0StateA);
    gm20b::Pipeline *b = server.pipeline(kTier0StateB);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    uint32_t vsProg = 0, fsProg = 0;
    ASSERT_TRUE(findMethod1(a->methods, kSetPipelineProgramVs, vsProg));
    ASSERT_TRUE(findMethod1(a->methods, kSetPipelineProgramFs, fsProg));
    EXPECT_EQ(vsProg, 0x30u);
    EXPECT_EQ(fsProg, 0x1030u);
    const std::vector<uint64_t> cbufs = findCbSelectorAddresses(a->methods);
    EXPECT_NE(std::find(cbufs.begin(), cbufs.end(), TGM_PACK_WINDOW_VA + 0x100u), cbufs.end());
    EXPECT_NE(std::find(cbufs.begin(), cbufs.end(), TGM_PACK_WINDOW_VA + 0x2000u), cbufs.end());

    drawState(server, a);
    drawState(server, b);
    EXPECT_TRUE(kernel_model::model.imports.empty());
    EXPECT_EQ(bindsInPackWindow(kernel_model::model), 0u);
    EXPECT_EQ(server.heapInPlaceBytes(), 0u);
    EXPECT_EQ(server.heapBytes(), 0u);

    gm20b::Device client;
    ASSERT_TRUE(client.open(pack.path.c_str(), kPackStack, err, gm20b::Device::Role::Client)) << err;
    ASSERT_EQ(kernel_model::model.others.size(), 1u);
    const kernel_model::Model &clientModel = *kernel_model::model.others.begin()->second;
    ASSERT_EQ(clientModel.packImageCalls.size(), 1u);
    EXPECT_TRUE(clientModel.packImageCalls[0].cpuRanges.empty());
    EXPECT_EQ(client.packWindowCpu(), nullptr);

    gm20b::Pipeline *clientA = client.pipeline(kTier0StateA);
    drawState(client, clientA);
    EXPECT_EQ(programRegionFromJobs(clientModel), TGM_PACK_WINDOW_VA);
    EXPECT_TRUE(clientModel.imports.empty());
    EXPECT_EQ(bindsInPackWindow(clientModel), 0u);
    EXPECT_EQ(client.heapInPlaceBytes(), 0u);
}

TEST(GM20BDevicePackWindow, RenderServerPassesAlignedTier0MetadataCpuRangesAndClientPassesNone)
{
    const BuiltPack pack = writeTwoTierPack("pack-window-cpu-ranges");
    configureReadyPackImage();

    gm20b::Device server;
    std::string err;
    ASSERT_TRUE(server.open(pack.path.c_str(), kPackStack, err, gm20b::Device::Role::RenderServer)) << err;
    ASSERT_EQ(kernel_model::model.packImageCalls.size(), 1u);
    const auto &ranges = kernel_model::model.packImageCalls[0].cpuRanges;
    ASSERT_FALSE(ranges.empty());
    EXPECT_LE(ranges.size(), size_t(TegraGM20BMaxPackCpuRanges));

    const uint64_t page = uint64_t(getpagesize());
    const uint64_t packBase = uint64_t(uintptr_t(server.pack().header()));
    const uint64_t heapStart = packBase + kHeapOffset;
    uint64_t totalBytes = 0;
    uint64_t prevEnd = 0;
    auto covers = [&](uint64_t addr, uint64_t len) {
        for (const TegraGM20BAbiPackCpuRange &r : ranges)
            if (addr >= r.address && addr + len <= r.address + r.bytes)
                return true;
        return false;
    };
    uint32_t wordsAt = 0, wordsBytes = 0;
    const bool words = server.pack().segmentWords(wordsAt, wordsBytes);
    const uint64_t wordsStart = (heapStart + wordsAt) & ~(page - 1);
    const uint64_t wordsEnd = (heapStart + wordsAt + wordsBytes + page - 1) & ~(page - 1);
    for (const TegraGM20BAbiPackCpuRange &r : ranges) {
        EXPECT_GT(r.bytes, 0u);
        EXPECT_EQ(r.address % page, 0u);
        EXPECT_EQ(r.bytes % page, 0u);
        EXPECT_GE(r.address, packBase);
        // the heap's pages are the kernel image's, but for the segments' words, which the CPU reads at draws
        if (r.address + r.bytes > heapStart)
            EXPECT_TRUE(words && r.address >= wordsStart && r.address + r.bytes <= wordsEnd);
        EXPECT_GT(r.address, prevEnd);
        prevEnd = r.address + r.bytes;
        totalBytes += r.bytes;
    }
    EXPECT_LE(totalBytes, 64ULL << 20);
    const gm20b::PackHeader &hdr = *server.pack().header();
    EXPECT_TRUE(covers(packBase + hdr.section[gm20b::PackFastKeys].offset, hdr.section[gm20b::PackFastKeys].bytes));
    EXPECT_TRUE(covers(packBase + hdr.section[gm20b::PackPipelines].offset, hdr.tier0[gm20b::PackPipelines]));
    EXPECT_TRUE(covers(packBase + hdr.section[gm20b::PackPrograms].offset, hdr.tier0[gm20b::PackPrograms]));

    gm20b::Device client;
    ASSERT_TRUE(client.open(pack.path.c_str(), kPackStack, err, gm20b::Device::Role::Client)) << err;
    ASSERT_EQ(kernel_model::model.others.size(), 1u);
    const kernel_model::Model &clientModel = *kernel_model::model.others.begin()->second;
    ASSERT_EQ(clientModel.packImageCalls.size(), 1u);
    EXPECT_TRUE(clientModel.packImageCalls[0].cpuRanges.empty());
    EXPECT_EQ(client.packWindowCpu(), nullptr);
}

TEST(GM20BDevicePackWindow, Tier1ProgramsImportGrainsOnDemandAfterPackWindow)
{
    const BuiltPack pack = writeTwoTierPack("pack-window-tier1");
    configureReadyPackImage();

    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(pack.path.c_str(), kPackStack, err, gm20b::Device::Role::Client)) << err;

    ASSERT_NE(dev.pipeline(kTier0StateA), nullptr);
    EXPECT_TRUE(kernel_model::model.imports.empty());
    EXPECT_EQ(dev.heapInPlaceBytes(), 0u);

    gm20b::Pipeline *t1a = dev.pipeline(kTier1StateA);
    ASSERT_NE(t1a, nullptr);
    EXPECT_EQ(kernel_model::model.imports.size(), 1u);
    EXPECT_EQ(dev.heapInPlaceBytes(), gm20b::kProgramGrain);
    EXPECT_EQ(dev.heapBytes(), 0u);
    const uint64_t grain0Va = TGM_PACK_WINDOW_VA + kTier0HeapBytes;
    ASSERT_EQ(kernel_model::model.binds.count(grain0Va), 1u);
    EXPECT_EQ(kernel_model::model.binds.at(grain0Va).second, gm20b::kProgramGrain);

    uint32_t vsProg = 0, fsProg = 0;
    ASSERT_TRUE(findMethod1(t1a->methods, kSetPipelineProgramVs, vsProg));
    ASSERT_TRUE(findMethod1(t1a->methods, kSetPipelineProgramFs, fsProg));
    EXPECT_EQ(vsProg, kTier0HeapBytes + 0x30u);
    EXPECT_EQ(fsProg, kTier0HeapBytes + 0x1030u);
    const std::vector<uint64_t> cbufs = findCbSelectorAddresses(t1a->methods);
    EXPECT_NE(std::find(cbufs.begin(), cbufs.end(), TGM_PACK_WINDOW_VA + kTier0HeapBytes + 0x2000u), cbufs.end());
    drawState(dev, t1a);

    gm20b::Pipeline *t1b = dev.pipeline(kTier1StateB);
    ASSERT_NE(t1b, nullptr);
    EXPECT_EQ(kernel_model::model.imports.size(), 1u);
    EXPECT_EQ(dev.heapInPlaceBytes(), gm20b::kProgramGrain);

    gm20b::Pipeline *t1c = dev.pipeline(kTier1StateC);
    ASSERT_NE(t1c, nullptr);
    EXPECT_EQ(kernel_model::model.imports.size(), 2u);
    EXPECT_EQ(dev.heapInPlaceBytes(), 2 * gm20b::kProgramGrain);
    EXPECT_EQ(kernel_model::model.binds.count(grain0Va + gm20b::kProgramGrain), 1u);
    drawState(dev, t1c);
}

TEST(GM20BDevicePackWindow, RefusedErrorAndMismatchUsePerPiecePlacement)
{
    const BuiltPack pack = writeTwoTierPack("pack-window-refused");

    const struct Case {
        const char *label;
        uint32_t status;
        int error;
        uint64_t stack;
    } cases[] = {
        {"refused", TegraGM20BPackStatusRefused, KERN_SUCCESS, kPackStack},
        {"kernel-error", TegraGM20BPackStatusReady, kIOReturnNoMemory, kPackStack},
        {"stack-mismatch", TegraGM20BPackStatusReady, KERN_SUCCESS, kPackStack ^ 1ull},
    };

    for (const Case &c : cases) {
        SCOPED_TRACE(c.label);
        configureReadyPackImage();
        kernel_model::model.packImage.status = c.status;
        kernel_model::model.packImage.error = c.error;
        kernel_model::model.packImage.stack = c.stack;

        gm20b::Device dev;
        std::string err;
        ASSERT_TRUE(dev.open(pack.path.c_str(), kPackStack, err, gm20b::Device::Role::Client)) << err;
        ASSERT_EQ(kernel_model::model.packImageCalls.size(), 1u);

        gm20b::Pipeline *a = dev.pipeline(kTier0StateA);
        ASSERT_NE(a, nullptr);
        EXPECT_FALSE(kernel_model::model.imports.empty());
        EXPECT_GT(dev.heapInPlaceBytes(), 0u);
        drawState(dev, a);
        const uint64_t region = programRegionFromJobs(kernel_model::model);
        EXPECT_NE(region, 0u);
        EXPECT_LT(region, TGM_PACK_WINDOW_VA);
    }
}

TEST(GM20BDevicePackWindow, GeneralVaAllocatorExcludesPackWindow)
{
    for (const uint64_t vaEnd : {uint64_t(1) << 39, uint64_t(1) << 40}) {
        kernel_model::model = {};
        kernel_model::model.vaStart = TGM_PACK_WINDOW_VA - 0x200000ull;
        kernel_model::model.vaEnd = vaEnd;
        kernel_model::model.packImage.enabled = true;

        gm20b::Kernel k;
        std::string err;
        ASSERT_TRUE(k.open(err)) << err;
        const uint64_t a = k.reserveVa(0x100000);
        const uint64_t b = k.reserveVa(0x100000);
        EXPECT_NE(a, 0u);
        EXPECT_NE(b, 0u);
        EXPECT_LE(a + 0x100000, TGM_PACK_WINDOW_VA);
        EXPECT_LE(b + 0x100000, TGM_PACK_WINDOW_VA);
        EXPECT_EQ(k.reserveVa(0x100000), 0u);
        k.releaseVa(a, 0x100000);
        k.releaseVa(b, 0x100000);
        EXPECT_EQ(k.reserveVa(0x400000), 0u);
    }

    kernel_model::model = {};
    kernel_model::model.vaStart = TGM_PACK_WINDOW_VA - 0x200000ull;
    kernel_model::model.vaEnd = uint64_t(1) << 39;
    kernel_model::model.packImage.enabled = false;
    gm20b::Kernel legacy;
    std::string err;
    ASSERT_TRUE(legacy.open(err)) << err;
    EXPECT_NE(legacy.reserveVa(0x400000), 0u);
}

TEST(GM20BDevicePackWindow, ConcurrentTier0AndTier1CreationUnderTSan)
{
    const BuiltPack pack = writeTwoTierPack("pack-window-tsan");
    configureReadyPackImage();

    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(pack.path.c_str(), kPackStack, err, gm20b::Device::Role::Client)) << err;

    constexpr size_t kThreads = 6;
    const std::array<uint64_t, 5> keys = {kTier0StateA, kTier0StateB, kTier1StateA, kTier1StateB, kTier1StateC};
    std::array<std::array<gm20b::Pipeline *, 5>, kThreads> got = {};
    std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (size_t t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int iter = 0; iter < 16; ++iter) {
                for (size_t k = 0; k < keys.size(); ++k) {
                    const size_t idx = (k + t + iter) % keys.size();
                    gm20b::Pipeline *pl = dev.pipeline(keys[idx]);
                    if (iter == 0)
                        got[t][idx] = pl;
                    else
                        EXPECT_EQ(got[t][idx], pl);
                }
                (void)dev.heapInPlaceBytes();
                (void)dev.heapBytes();
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (std::thread &w : workers)
        w.join();

    for (size_t k = 0; k < keys.size(); ++k) {
        ASSERT_NE(got[0][k], nullptr);
        for (size_t t = 1; t < kThreads; ++t)
            EXPECT_EQ(got[t][k], got[0][k]);
    }
    EXPECT_EQ(kernel_model::model.imports.size(), 2u);
    EXPECT_EQ(dev.heapInPlaceBytes(), 2 * gm20b::kProgramGrain);
    drawState(dev, got[0][0]);
    drawState(dev, got[0][2]);
}

class IoctlGate {
public:
    explicit IoctlGate(uint32_t method) : method_(method)
    {
        std::lock_guard<std::mutex> l(mu_);
        active_ = this;
        kernel_model::arriving.store(&arrive, std::memory_order_release);
    }

    ~IoctlGate()
    {
        release();
        kernel_model::arriving.store(nullptr, std::memory_order_release);
        std::lock_guard<std::mutex> l(mu_);
        if (active_ == this)
            active_ = nullptr;
    }

    bool waitHeld()
    {
        std::unique_lock<std::mutex> l(mu_);
        return cv_.wait_for(l, std::chrono::seconds(5), [&] {
            return held_;
        });
    }

    void release()
    {
        std::lock_guard<std::mutex> l(mu_);
        released_ = true;
        cv_.notify_all();
    }

private:
    static int arrive(uint32_t method, const void *)
    {
        std::unique_lock<std::mutex> l(mu_);
        IoctlGate *g = active_;
        if (!g || method != g->method_ || g->matched_)
            return KERN_SUCCESS;
        g->matched_ = true;
        g->held_ = true;
        g->cv_.notify_all();
        g->cv_.wait(l, [&] {
            return g->released_;
        });
        return KERN_SUCCESS;
    }

    uint32_t method_ = 0;
    bool matched_ = false, held_ = false, released_ = false;
    std::condition_variable cv_;
    static inline std::mutex mu_;
    static inline IoctlGate *active_ = nullptr;
};

TEST(GM20BDevicePackWindow, PlaceInPlaceDropsDeviceLockDuringColdGrainImportAndDeduplicates)
{
    const BuiltPack pack = writeTwoTierPack("pack-window-offlock-grain");
    configureReadyPackImage();

    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(pack.path.c_str(), kPackStack, err, gm20b::Device::Role::Client)) << err;

    IoctlGate gate(TegraGM20BMethodBoImport);
    gm20b::Pipeline *p1 = nullptr, *p2 = nullptr;
    std::thread t1([&] {
        p1 = dev.pipeline(kTier1StateA);
    });
    ASSERT_TRUE(gate.waitHeld());

    EXPECT_TRUE(dev.lockFree());
    gm20b::Buffer buf;
    ASSERT_TRUE(dev.createBuffer(4096, buf));
    dev.destroyBuffer(buf);
    EXPECT_NE(dev.pipeline(kTier0StateA), nullptr);

    std::thread t2([&] {
        p2 = dev.pipeline(kTier1StateA);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    EXPECT_TRUE(dev.lockFree());

    gate.release();
    t1.join();
    t2.join();
    ASSERT_NE(p1, nullptr);
    EXPECT_EQ(p1, p2);
    EXPECT_EQ(kernel_model::model.imports.size(), 1u);
}

TEST(GM20BDevicePackWindow, HeapGrowAndSlmNeedDropDeviceLockDuringIoctls)
{
    const BuiltPack pack = writeTwoTierPack("pack-window-offlock-heap-slm", 64);
    kernel_model::model = {};
    kernel_model::model.packImage.enabled = false;

    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(pack.path.c_str(), kPackStack, err, gm20b::Device::Role::Client)) << err;
    // Warm up one pool chunk so the concurrent createBuffer probe below exercises Device::lock_ without triggering a
    // second BoAlloc/BoImport of its own.
    gm20b::Buffer warm;
    ASSERT_TRUE(dev.createBuffer(4096, warm));
    dev.destroyBuffer(warm);

    {
        IoctlGate importGate(TegraGM20BMethodBoImport);
        gm20b::Pipeline *pl = nullptr;
        std::thread worker([&] {
            pl = dev.pipeline(kTier1StateA);
        });
        ASSERT_TRUE(importGate.waitHeld());
        EXPECT_TRUE(dev.lockFree());
        gm20b::Buffer probe;
        ASSERT_TRUE(dev.createBuffer(4096, probe));
        dev.destroyBuffer(probe);
        importGate.release();
        worker.join();
        ASSERT_NE(pl, nullptr);
    }

    const BuiltPack packSlm = writeTwoTierPack("pack-window-offlock-slm", 128);
    kernel_model::model = {};
    kernel_model::model.packImage.enabled = false;
    gm20b::Device devSlm;
    ASSERT_TRUE(devSlm.open(packSlm.path.c_str(), kPackStack, err, gm20b::Device::Role::Client)) << err;
    gm20b::Buffer warmSlm;
    ASSERT_TRUE(devSlm.createBuffer(4096, warmSlm));
    devSlm.destroyBuffer(warmSlm);

    IoctlGate allocGate(TegraGM20BMethodBoAlloc);
    gm20b::Pipeline *pl = nullptr;
    std::thread worker([&] {
        pl = devSlm.pipeline(kTier1StateA);
    });
    ASSERT_TRUE(allocGate.waitHeld());
    EXPECT_TRUE(devSlm.lockFree());
    gm20b::Buffer probe;
    ASSERT_TRUE(devSlm.createBuffer(4096, probe));
    devSlm.destroyBuffer(probe);
    allocGate.release();
    worker.join();
    ASSERT_NE(pl, nullptr);
}

}  // namespace
