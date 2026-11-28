// SPDX-License-Identifier: MIT
// Stage 3 pack-hit verification: pre-built state segments in pack v34, lock-free zero-kernel-call first/repeat tier-0
// creation at both gm20b::Device and SwitchMetalDevice layers, exact GM20B method-stream equivalence vs dynamic builder,
// tier-1 / runtime-compiled fallback cleanup, and 8-thread TSan concurrency stress.
#include "facade_harness.h"
#include "pack_fixture_image.h"
#include "bazel/testing/fresh_process.h"
#include "../blend_kill.h"
#include "../creation_cost.h"
#include "../encode_profile.h"
#include "../lock.h"
#include "../pack_scan.h"
#include "../pack_window.h"
#include "../record.h"
#include "../state_segment.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

@interface SwitchMetalDevice (PackHitTestCost)
- (NSDictionary<NSString *, NSNumber *> *)switchMetalLastCreation;
@end

namespace {

constexpr const char *kFixturePath = pack_fixture::kPath;

constexpr const char *kPlainKey = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v v  | f f";
constexpr const char *kBlendInterlockKey = "samples=2 topo=0 ds=0/0 c0=80:15:1:4:5:0:4:5:0  | v blend_v  | f blend_f";
constexpr const char *kNoPixelRast0Key = "samples=1 topo=0 ds=0/0 rast=0  | v nopix_v  | f -";
constexpr const char *kTessKey = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v tess_v  | f tess_f";
constexpr const char *kFetchKey = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v fetch_v  | f fetch_f";
constexpr const char *kMatrixKey = "samples=1 topo=0 ds=0/0 c0=115:15:0:1:0:0:1:0:0  | v mat_v  | f mat_f";
constexpr const char *kVariantsKey = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v var_v  | f var_f";
constexpr const char *kStrideMainKey = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v stride_v  | f stride_f";
constexpr const char *kPassWriteKey = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v passw_v  | f passw_f";
constexpr const char *kTier1Key = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v cold_v  | f cold_f";

constexpr uint32_t kCbSelectorA = 0x2380;
constexpr uint32_t kCbSelectorB = 0x2384;
constexpr uint32_t kCbSelectorC = 0x2388;
constexpr uint32_t kRootBytes = 0x800;

uint64_t hashKey(const char *key)
{
    return gm20b::Pack::fnv(key, std::strlen(key));
}

using pack_fixture::configurePackImageFromBytes;
using pack_fixture::readFileBytes;

struct DecodedMethod {
    uint16_t subc;
    uint16_t method;
    uint32_t value;

    bool operator==(const DecodedMethod &o) const
    {
        return subc == o.subc && method == o.method && value == o.value;
    }
};

// Decode a push stream into hardware-equivalent (subc, method, value) tuples:
// - normalizes per-stage SET_PIPELINE_PROGRAM offsets (prebuilt uses pack heapOffset; dynamic bump-allocates in heap_)
//   and checks CbufShaderData GPU VAs match each stage's placed program base + dataOffset,
// - simulates the GM20B CB_SELECTOR register to verify every BIND_GROUP_CB targets shader data and the stream ends with
//   CB_SELECTOR restored to rootVa (deferred via reselectRoot in prebuilt vs immediate per-cbuf in dynamic).
std::vector<DecodedMethod> decodeNormalized(const uint32_t *words, uint32_t count, bool appendRootReselect,
                                            uint64_t heapVa, uint64_t rootVa)
{
    std::vector<uint32_t> full(words, words + count);
    if (appendRootReselect) {
        uint32_t tail[4] = {};
        gm20b::Push p{tail, tail + 4};
        p.inc(kCbSelectorA, {kRootBytes, uint32_t(rootVa >> 32), uint32_t(rootVa)});
        full.insert(full.end(), tail, p.at);
    }
    std::vector<switch_gm20b_method> raw(full.size() * 2 + 8);
    const long n = switch_gm20b_push_decode(full.data(), full.size(), raw.data(), raw.size());
    EXPECT_GE(n, 0);

    auto progMethodStage = [](uint16_t m) -> int {
        switch (m) {
        case 0x2044:
            return 0;  // vertex
        case 0x2144:
            return 1;  // pixel
        case 0x2084:
            return 2;  // tess control
        case 0x20c4:
            return 3;  // tess eval
        default:
            return -1;
        }
    };
    auto bindMethodStage = [](uint16_t m) -> int {
        switch (m) {
        case 0x2410:
            return 0;  // group 0: vertex
        case 0x2490:
            return 1;  // group 4: pixel
        case 0x2430:
            return 2;  // group 1: tess control
        case 0x2450:
            return 3;  // group 2: tess eval
        default:
            return -1;
        }
    };

    std::array<uint64_t, 4> stageBase = {};
    std::array<bool, 4> haveStage = {};
    bool cbIsRoot = true;
    std::vector<DecodedMethod> out;
    out.reserve(size_t(std::max<long>(0, n)));
    for (long i = 0; i < n; ++i) {
        if (raw[i].subchannel == 0) {
            if (const int st = progMethodStage(raw[i].method); st >= 0) {
                const uint32_t addr = raw[i].value;
                stageBase[size_t(st)] = addr & ~uint32_t(0xFF);
                haveStage[size_t(st)] = true;
                out.push_back({0, raw[i].method, addr & 0xFFu});
                continue;
            }
            if (i + 2 < n && raw[i].method == kCbSelectorA && raw[i + 1].method == kCbSelectorB &&
                raw[i + 2].method == kCbSelectorC) {
                const uint32_t size = raw[i].value;
                const uint64_t va = (uint64_t(raw[i + 1].value) << 32) | raw[i + 2].value;
                if (va == rootVa && size == kRootBytes) {
                    cbIsRoot = true;
                    i += 2;
                    continue;
                }
                cbIsRoot = false;
                EXPECT_GE(va, heapVa);
                uint64_t rel = va - heapVa;
                if (i + 3 < n && raw[i + 3].subchannel == 0) {
                    const int st = bindMethodStage(raw[i + 3].method);
                    EXPECT_GE(st, 0);
                    if (st >= 0 && haveStage[size_t(st)]) {
                        EXPECT_GE(rel, stageBase[size_t(st)]);
                        rel -= stageBase[size_t(st)];
                    }
                }
                out.push_back({0, kCbSelectorA, size});
                out.push_back({0, kCbSelectorB, uint32_t(rel >> 32)});
                out.push_back({0, kCbSelectorC, uint32_t(rel)});
                i += 2;
                continue;
            }
            if (bindMethodStage(raw[i].method) >= 0)
                EXPECT_FALSE(cbIsRoot);
        }
        out.push_back({raw[i].subchannel, raw[i].method, raw[i].value});
    }
    EXPECT_TRUE(cbIsRoot);
    return out;
}

struct CreationSnapshot {
    uint64_t locks = 0, lockWaits = 0, lockWaitTicks = 0;
    uint64_t controlAllocs = 0, bos = 0, imports = 0, binds = 0;
    gm20b::CreationWork work;
};

CreationSnapshot takeSnapshot()
{
    CreationSnapshot s;
    gm20b::LockCount::here(s.locks, s.lockWaits, s.lockWaitTicks);
    s.controlAllocs = kernel_model::model.controlAllocs;
    s.bos = kernel_model::model.bos.size();
    s.imports = kernel_model::model.imports.size();
    s.binds = kernel_model::model.binds.size();
    s.work = gm20b::tCreationWork;
    return s;
}

TEST(GM20BPackHitStage3, PackScanAcceptsGeneratedV34PackAndHeaderMetadata)
{
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(kFixturePath, 0, err)) << err;
    const std::vector<std::string> problems = gm20b::packScan(pack);
    EXPECT_TRUE(problems.empty()) << (problems.empty() ? "" : problems[0]);

    const gm20b::PackHeader &h = *pack.header();
    EXPECT_EQ(h.version, gm20b::kPackVersion);
    EXPECT_EQ(h.tier0SlmPerWarp, 64u * 32u + 512u);
    const uint64_t tier0Pipes = pack.tier0Entries(gm20b::PackPipelines, sizeof(gm20b::PackPipeline));
    ASSERT_GT(tier0Pipes, 0u);
    for (uint32_t i = 0; i < tier0Pipes; ++i) {
        const gm20b::PackSegment *seg = pack.pipelineSegment(i);
        ASSERT_NE(seg, nullptr) << "tier-0 pipeline " << i;
        EXPECT_TRUE(gm20b::packWindowContainsSegment(*seg, h.tier0[gm20b::PackHeap]));
        EXPECT_EQ(seg->heapOffset & 15u, 0u);
        EXPECT_GT(seg->words, gm20b::kSegmentBindWords);
    }
}

TEST(GM20BPackHitStage3, FirstAndRepeatTier0HitsAreLockFreeAndZeroKernelCalls)
{
    const std::vector<uint8_t> file = readFileBytes(kFixturePath);
    ASSERT_FALSE(file.empty());
    configurePackImageFromBytes(file, true);

    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(kFixturePath, 0, err, gm20b::Device::Role::Client)) << err;
    ASSERT_GT(dev.packWindowBytes(), 0u);
    dev.probeFetchRung.store(3, std::memory_order_relaxed);

    const std::array<std::pair<const char *, gm20b::PackForm>, 9> tier0Keys = {{
        {kPlainKey, gm20b::PackForm::Default},
        {kBlendInterlockKey, gm20b::PackForm::Default},
        {kNoPixelRast0Key, gm20b::PackForm::Default},
        {kTessKey, gm20b::PackForm::Default},
        {kFetchKey, gm20b::PackForm::Default},
        {kMatrixKey, gm20b::PackForm::FetchFree},
        {kVariantsKey, gm20b::PackForm::Default},
        {kStrideMainKey, gm20b::PackForm::Default},
        {kPassWriteKey, gm20b::PackForm::Default},
    }};

    gm20b::EncodeProfile profile;
    gm20b::EncodeProfile *saved = gm20b::tEncodeProfile;
    gm20b::tEncodeProfile = &profile;
    const CreationSnapshot beforeFirst = takeSnapshot();

    std::vector<gm20b::Pipeline *> first;
    first.reserve(tier0Keys.size());
    for (const auto &[key, form] : tier0Keys) {
        const uint64_t h = hashKey(key);
        const gm20b::PackRun run = dev.pack().fastRun(h);
        ASSERT_GT(run.records, 0u) << key;
        gm20b::Pipeline *pl = dev.pipeline(run, h, form, false);
        ASSERT_NE(pl, nullptr) << key;
        EXPECT_TRUE(pl->methods.empty()) << key;
        ASSERT_NE(pl->prebuiltWords, nullptr) << key;
        EXPECT_GT(pl->prebuiltWordCount, gm20b::kSegmentBindWords) << key;
        EXPECT_GE(pl->segment.va, TGM_PACK_WINDOW_VA);
        EXPECT_LT(pl->segment.va, TGM_PACK_WINDOW_VA + dev.packWindowBytes());
        (void)dev.variantPipelines(run, false);
        first.push_back(pl);
    }

    const CreationSnapshot afterFirst = takeSnapshot();
    gm20b::tEncodeProfile = saved;

    EXPECT_EQ(afterFirst.locks - beforeFirst.locks, 0u);
    EXPECT_EQ(afterFirst.lockWaits - beforeFirst.lockWaits, 0u);
    EXPECT_EQ(profile.calls[size_t(gm20b::EncodePart::Kernel)], 0u);
    EXPECT_EQ(afterFirst.controlAllocs - beforeFirst.controlAllocs, 0u);
    EXPECT_EQ(afterFirst.bos - beforeFirst.bos, 0u);
    EXPECT_EQ(afterFirst.imports - beforeFirst.imports, 0u);
    EXPECT_EQ(afterFirst.binds - beforeFirst.binds, 0u);
    EXPECT_EQ(afterFirst.work.lookups - beforeFirst.work.lookups, 0u);
    EXPECT_EQ(afterFirst.work.pieces - beforeFirst.work.pieces, 0u);
    EXPECT_EQ(afterFirst.work.copies - beforeFirst.work.copies, 0u);
    EXPECT_EQ(afterFirst.work.copiedBytes - beforeFirst.work.copiedBytes, 0u);
    EXPECT_GT(afterFirst.work.builds - beforeFirst.work.builds, 0u);
    EXPECT_EQ(dev.heapBytes(), 0u);
    EXPECT_EQ(dev.heapInPlaceBytes(), 0u);

    // Verify specific state features on the lock-free built objects.
    EXPECT_TRUE(first[0]->reselectRoot);
    EXPECT_TRUE(first[1]->blendKill);
    EXPECT_TRUE(first[2]->stagesOff);
    EXPECT_FALSE(first[2]->restore.empty());
    ASSERT_NE(first[4]->barrierTwin, nullptr);
    EXPECT_TRUE(first[4]->barrierTwin->methods.empty());
    ASSERT_NE(first[4]->fetchFree, nullptr);
    EXPECT_TRUE(first[4]->fetchFree->methods.empty());
    ASSERT_NE(first[7]->strideTwin, nullptr);
    EXPECT_TRUE(first[7]->strideTwin->methods.empty());
    EXPECT_FALSE(first[8]->segment.keptAcrossPasses);
    EXPECT_TRUE(first[0]->segment.keptAcrossPasses);

    // Repeat creation: zero locks, zero kernel calls, zero builds, identical pointers.
    profile = {};
    gm20b::tEncodeProfile = &profile;
    const CreationSnapshot beforeRepeat = takeSnapshot();
    for (size_t i = 0; i < tier0Keys.size(); ++i) {
        const uint64_t h = hashKey(tier0Keys[i].first);
        const gm20b::PackRun run = dev.pack().fastRun(h);
        gm20b::Pipeline *pl = dev.pipeline(run, h, tier0Keys[i].second, false);
        EXPECT_EQ(pl, first[i]);
        (void)dev.variantPipelines(run, false);
    }
    const CreationSnapshot afterRepeat = takeSnapshot();
    gm20b::tEncodeProfile = saved;

    EXPECT_EQ(afterRepeat.locks - beforeRepeat.locks, 0u);
    EXPECT_EQ(profile.calls[size_t(gm20b::EncodePart::Kernel)], 0u);
    EXPECT_EQ(afterRepeat.work.builds - beforeRepeat.work.builds, 0u);
    EXPECT_EQ(afterRepeat.work.lookups - beforeRepeat.work.lookups, 0u);
}

// The fixture with every segment's kill word made NOOP_NEVER (`words`) or every segment's kill flag cleared (`flag`),
// written where a Device opens it, its image the kernel model's
std::string patchedKillPack(bool words, bool flag)
{
    std::vector<uint8_t> bytes = readFileBytes(kFixturePath);
    EXPECT_FALSE(bytes.empty());
    gm20b::PackHeader h = {};
    std::memcpy(&h, bytes.data(), sizeof h);
    const gm20b::PackSection segments = h.section[gm20b::PackSegments];
    gm20b::PackSegmentTable table = {};
    std::memcpy(&table, bytes.data() + segments.offset, sizeof table);
    constexpr uint32_t kKillWord =
        gm20b::Push::hdr(4, gm20b::SUBC_3D, gm20b::kIteratedBlendOptimization, gm20b::kNoopSourceRgba0000);
    uint32_t killing = 0;
    for (uint32_t i = 0; i < table.pipelineCount; i++) {
        uint8_t *at = bytes.data() + segments.offset + sizeof table + i * sizeof(gm20b::PackSegment);
        gm20b::PackSegment segment = {};
        std::memcpy(&segment, at, sizeof segment);
        killing += (segment.flags & gm20b::kPackSegmentBlendKill) != 0;
        if (flag) {
            segment.flags &= uint16_t(~gm20b::kPackSegmentBlendKill);
            std::memcpy(at, &segment, sizeof segment);
        }
        auto *w = reinterpret_cast<uint32_t *>(bytes.data() + h.section[gm20b::PackHeap].offset + segment.heapOffset);
        for (uint32_t k = 0; words && k < segment.words; k++)
            if (w[k] == kKillWord)
                w[k] = gm20b::Push::hdr(4, gm20b::SUBC_3D, gm20b::kIteratedBlendOptimization, gm20b::kNoopNever);
    }
    EXPECT_GT(killing, 0u) << "no segment of the fixture is flagged to kill";
    const std::string path = harness::dir() + (flag ? "/pack_v34_no_kill_flag.bin" : "/pack_v34_no_kill_words.bin");
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
    configurePackImageFromBytes(bytes, true);
    return path;
}

// A prebuilt state's zero-source kill is its segment's flag (pack.h kPackSegmentBlendKill), in the segment table tier 0
// wires: its creation reads none of the segment's words, heap pages no process keeps resident. A pack built before a
// rule change may decide otherwise than the runtime's rule: the state is what the GPU gets, and the disagreement is
// counted.
TEST(GM20BPackHitStage3, APrebuiltStatesKillIsItsSegmentsFlagNotItsWords)
{
    const uint64_t key = hashKey(kBlendInterlockKey);
    {
        const std::string path = patchedKillPack(true, false);
        gm20b::Device dev;
        std::string err;
        ASSERT_TRUE(dev.open(path.c_str(), 0, err, gm20b::Device::Role::Client)) << err;
        gm20b::Pipeline *pl = dev.pipeline(dev.pack().fastRun(key), key, gm20b::PackForm::Default, false);
        ASSERT_NE(pl, nullptr);
        EXPECT_TRUE(pl->blendKill) << "the flag says kill; the words were never read";
        EXPECT_EQ(dev.stats.blendKillPackDisagreements.load(), 0u);
    }
    const std::string path = patchedKillPack(false, true);
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(path.c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    gm20b::Pipeline *pl = dev.pipeline(dev.pack().fastRun(key), key, gm20b::PackForm::Default, false);
    ASSERT_NE(pl, nullptr);
    ASSERT_NE(pl->prebuiltWords, nullptr);
    EXPECT_FALSE(pl->blendKill) << "the flag says no kill";
    EXPECT_EQ(dev.stats.blendKillStates.load(), 0u);
    EXPECT_EQ(dev.stats.blendKillPackDisagreements.load(), 1u) << "the runtime's rule kills this state";
}

TEST(GM20BPackHitStage3, PrebuiltStateSegmentsMatchDynamicBuilderExactlyAndDrawFromPackWindow)
{
    const std::vector<uint8_t> file = readFileBytes(kFixturePath);
    ASSERT_FALSE(file.empty());

    // Device A: pre-built v34 state segments from shared pack window.
    configurePackImageFromBytes(file, true);
    gm20b::Device prebuiltDev;
    std::string err;
    ASSERT_TRUE(prebuiltDev.open(kFixturePath, 0, err, gm20b::Device::Role::Client)) << err;
    const uint64_t rootVaA = 0x1000000000ull;  // dummy distinct root tag for stream normalization check

    // Device B: pack window disabled -> dynamic Device::build + StateSegmentHeap.
    kernel_model::model.packImage.enabled = false;
    gm20b::Device dynamicDev;
    ASSERT_TRUE(dynamicDev.open(kFixturePath, 0, err, gm20b::Device::Role::Client)) << err;

    // Extract dynamicDev's actual heapVa and rootVa from a submitted context job.
    ASSERT_TRUE(dynamicDev.ensureChannel());
    const kernel_model::Model &dynModel = *kernel_model::model.others.begin()->second;
    uint64_t dynHeapVa = 0, dynRootVa = 0;
    {
        ASSERT_FALSE(dynModel.jobs.empty());
        const auto &w = dynModel.jobs[0].words;
        std::vector<switch_gm20b_method> ms(w.size() * 2 + 8);
        const long n = switch_gm20b_push_decode(w.data(), w.size(), ms.data(), ms.size());
        uint32_t hHi = 0, hLo = 0;
        for (long i = 0; i < n; ++i) {
            if (ms[i].subchannel == 0 && ms[i].method == 0x1608)
                hHi = ms[i].value;
            if (ms[i].subchannel == 0 && ms[i].method == 0x160c)
                hLo = ms[i].value;
            if (ms[i].subchannel == 0 && ms[i].method == kCbSelectorA && ms[i].value == kRootBytes && i + 2 < n &&
                ms[i + 1].method == kCbSelectorB && ms[i + 2].method == kCbSelectorC)
                dynRootVa = (uint64_t(ms[i + 1].value) << 32) | ms[i + 2].value;
        }
        dynHeapVa = (uint64_t(hHi) << 32) | hLo;
    }
    ASSERT_NE(dynHeapVa, 0u);
    ASSERT_NE(dynRootVa, 0u);

    auto comparePipelines = [&](const char *label, gm20b::Pipeline *a, gm20b::Pipeline *b) {
        SCOPED_TRACE(label);
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);
        EXPECT_TRUE(a->methods.empty());
        ASSERT_NE(a->prebuiltWords, nullptr);
        EXPECT_FALSE(b->methods.empty());

        const auto streamA =
            decodeNormalized(a->methodWords(), a->methodCount(), a->reselectRoot, TGM_PACK_WINDOW_VA, rootVaA);
        const auto streamB = decodeNormalized(b->methodWords(), b->methodCount(), false, dynHeapVa, dynRootVa);
        EXPECT_EQ(streamA, streamB);
        EXPECT_EQ(a->segment.keptAcrossPasses, b->segment.keptAcrossPasses);
        EXPECT_EQ(a->restore, b->restore);
        EXPECT_EQ(a->blendKill, b->blendKill);
        EXPECT_EQ(a->writeBits, b->writeBits);
        EXPECT_EQ(a->readBits, b->readBits);
        EXPECT_EQ(a->stagesOff, b->stagesOff);
        EXPECT_EQ(a->ubosNamed, b->ubosNamed);
        EXPECT_EQ(a->uboCount, b->uboCount);
        EXPECT_EQ(a->freeForm.rung, b->freeForm.rung);
        EXPECT_EQ(a->freeForm.colour, b->freeForm.colour);
        EXPECT_EQ(a->freeForm.matrixSlot, b->freeForm.matrixSlot);
        for (int s = 0; s < 4; ++s) {
            EXPECT_EQ(a->setSlot[s], b->setSlot[s]);
            EXPECT_EQ(a->heapSlot[s], b->heapSlot[s]);
        }
    };

    // Enable probeFetchRung=3 so fetch-free iterated blend form is placed too.
    prebuiltDev.probeFetchRung.store(3, std::memory_order_relaxed);
    dynamicDev.probeFetchRung.store(3, std::memory_order_relaxed);

    const std::array<std::pair<const char *, gm20b::PackForm>, 11> cases = {{
        {kPlainKey, gm20b::PackForm::Default},
        {kBlendInterlockKey, gm20b::PackForm::Default},
        {kNoPixelRast0Key, gm20b::PackForm::Default},
        {kTessKey, gm20b::PackForm::Default},
        {kFetchKey, gm20b::PackForm::Default},
        {kFetchKey, gm20b::PackForm::Barrier},
        {kFetchKey, gm20b::PackForm::FetchFree},
        {kMatrixKey, gm20b::PackForm::FetchFree},
        {kVariantsKey, gm20b::PackForm::Default},
        {kStrideMainKey, gm20b::PackForm::Default},
        {kPassWriteKey, gm20b::PackForm::Default},
    }};
    for (const auto &[key, form] : cases) {
        const uint64_t h = hashKey(key);
        gm20b::Pipeline *a = prebuiltDev.pipeline(prebuiltDev.pack().fastRun(h), h, form, false);
        gm20b::Pipeline *b = dynamicDev.pipeline(dynamicDev.pack().fastRun(h), h, form, false);
        comparePipelines(key, a, b);
    }

    // Verify program-variant prebuilt segments match dynamic builder too.
    {
        const uint64_t h = hashKey(kVariantsKey);
        gm20b::Pipeline *baseA =
            prebuiltDev.pipeline(prebuiltDev.pack().fastRun(h), h, gm20b::PackForm::Default, false);
        gm20b::Pipeline *baseB = dynamicDev.pipeline(dynamicDev.pack().fastRun(h), h, gm20b::PackForm::Default, false);
        comparePipelines("variant-aligned", prebuiltDev.variant(*baseA, gm20b::AssumeUniformAligned),
                         dynamicDev.variant(*baseB, gm20b::AssumeUniformAligned));
        comparePipelines("variant-turned", prebuiltDev.variant(*baseA, gm20b::AssumeTurned),
                         dynamicDev.variant(*baseB, gm20b::AssumeTurned));
    }

    // Submit a real draw with the prebuilt CbufShaderData state and confirm GPFIFO push targets pack window + reselects root.
    gm20b::Pipeline *plainA = prebuiltDev.pipeline(prebuiltDev.pack().fastRun(hashKey(kPlainKey)), hashKey(kPlainKey),
                                                   gm20b::PackForm::Default, false);
    ASSERT_NE(plainA, nullptr);
    gm20b::Texture rt;
    ASSERT_TRUE(prebuiltDev.createTexture(16, 16, 80, rt, false, true));
    gm20b::PassDesc pass;
    pass.count = 1;
    pass.color[0].texture = &rt;
    pass.color[0].load = gm20b::ColorAttachment::Clear;
    pass.color[0].store = true;
    gm20b::CommandBuffer *cb = prebuiltDev.begin();
    ASSERT_NE(cb, nullptr);
    const char *why = nullptr;
    gm20b::RenderEncoder *re = cb->render(pass, &why);
    ASSERT_NE(re, nullptr) << (why ? why : "");
    re->setPipeline(plainA);
    ASSERT_TRUE(re->draw(3, 0, 3, 1, 0, &why)) << (why ? why : "");
    re->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    EXPECT_GT(seqno, 0u);
    ASSERT_FALSE(kernel_model::model.jobs.empty());
    const kernel_model::Job &drawJob = kernel_model::model.jobs.back();
    bool sawPackWindowPush = false;
    for (const TegraGM20BAbiPush &push : drawJob.pushes)
        if (push.va == plainA->segment.va && push.dwords == plainA->segment.words)
            sawPackWindowPush = true;
    EXPECT_TRUE(sawPackWindowPush);
    prebuiltDev.destroyTexture(rt);
}

TEST(GM20BPackHitStage3, FacadeFirstAndRepeatTier0PackHitMeetPackHitCostContract)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        // Copy fixture into TEST_TMPDIR and patch kPlainKey's fast key to match QuartzCore descriptor(BGRA8Unorm).
        std::vector<uint8_t> bytes = readFileBytes(kFixturePath);
        ASSERT_FALSE(bytes.empty());
        gm20b::PackHeader h = {};
        std::memcpy(&h, bytes.data(), sizeof h);
        const std::string qcKey = harness::packKey(MTLPixelFormatBGRA8Unorm);
        ASSERT_EQ(qcKey, kPlainKey);
        const uint64_t wantFast = harness::fastKeyOfText(qcKey);
        const uint64_t plainHash = hashKey(kPlainKey);
        const uint64_t fkOff = h.section[gm20b::PackFastKeys].offset;
        const uint64_t fkTier0 = h.tier0[gm20b::PackFastKeys] / sizeof(gm20b::PackFastKey);
        auto *fks = reinterpret_cast<gm20b::PackFastKey *>(bytes.data() + fkOff);
        bool patched = false;
        for (uint64_t i = 0; i < fkTier0; ++i) {
            if (fks[i].fast == plainHash) {
                fks[i].fast = wantFast;
                patched = true;
                break;
            }
        }
        ASSERT_TRUE(patched);
        std::sort(fks, fks + fkTier0, [](const gm20b::PackFastKey &a, const gm20b::PackFastKey &b) {
            return a.fast < b.fast;
        });

        const std::string packPath = harness::dir() + "/pack_v34_facade.bin";
        std::ofstream(packPath, std::ios::binary)
            .write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));

        configurePackImageFromBytes(bytes, true);
        id<MTLDevice> device = harness::open(packPath);
        ASSERT_TRUE(device != nil);

        NSError *error = nil;
        id<MTLLibrary> library =
            [device newLibraryWithURL:[NSURL fileURLWithPath:@(harness::writeMetallib().c_str())] error:&error];
        ASSERT_TRUE(library != nil) << (error ? error.localizedDescription.UTF8String : "");

        MTLRenderPipelineDescriptor *desc = harness::descriptor(library, MTLPixelFormatBGRA8Unorm);
        desc.label = @"com.apple.coreanimation.PBGRAXm_TimgA2Xhf_Isrc";

        id<MTLRenderPipelineState> firstState = [device newRenderPipelineStateWithDescriptor:desc error:&error];
        ASSERT_TRUE(firstState != nil) << (error ? error.localizedDescription.UTF8String : "");
        EXPECT_TRUE(((SwitchMetalPipeline *)firstState)->_packed);
        NSDictionary<NSString *, NSNumber *> *firstCost = [(SwitchMetalDevice *)device switchMetalLastCreation];
        ASSERT_TRUE(firstCost != nil);
        EXPECT_EQ(firstCost[@"lookups"].unsignedLongLongValue, 0u);
        EXPECT_EQ(firstCost[@"kernel_calls"].unsignedLongLongValue, 0u);
        EXPECT_EQ(firstCost[@"locks"].unsignedLongLongValue, 0u);
        EXPECT_EQ(firstCost[@"lock_waits"].unsignedLongLongValue, 0u);
        EXPECT_EQ(firstCost[@"copied_bytes"].unsignedLongLongValue, 0u);
        EXPECT_EQ(firstCost[@"copies"].unsignedLongLongValue, 0u);
        EXPECT_EQ(firstCost[@"pieces"].unsignedLongLongValue, 0u);
        EXPECT_EQ(firstCost[@"text_keys"].unsignedLongLongValue, 0u);
        EXPECT_GE(firstCost[@"builds"].unsignedLongLongValue, 1u);

        id<MTLRenderPipelineState> repeatState = [device newRenderPipelineStateWithDescriptor:desc error:&error];
        ASSERT_TRUE(repeatState != nil);
        EXPECT_EQ(((SwitchMetalPipeline *)repeatState)->_raw, ((SwitchMetalPipeline *)firstState)->_raw);
        NSDictionary<NSString *, NSNumber *> *repeatCost = [(SwitchMetalDevice *)device switchMetalLastCreation];
        ASSERT_TRUE(repeatCost != nil);
        EXPECT_EQ(repeatCost[@"lookups"].unsignedLongLongValue, 0u);
        EXPECT_EQ(repeatCost[@"kernel_calls"].unsignedLongLongValue, 0u);
        EXPECT_EQ(repeatCost[@"locks"].unsignedLongLongValue, 0u);
        EXPECT_EQ(repeatCost[@"copied_bytes"].unsignedLongLongValue, 0u);
        EXPECT_EQ(repeatCost[@"pieces"].unsignedLongLongValue, 0u);
        EXPECT_EQ(repeatCost[@"text_keys"].unsignedLongLongValue, 0u);
        EXPECT_EQ(repeatCost[@"builds"].unsignedLongLongValue, 0u);
    });
}

TEST(GM20BPackHitStage3, Tier1ColdAndRuntimeCompiledFallbackWorkAndCleanUp)
{
    const std::vector<uint8_t> file = readFileBytes(kFixturePath);
    ASSERT_FALSE(file.empty());
    configurePackImageFromBytes(file, true);

    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(kFixturePath, 0, err, gm20b::Device::Role::Client)) << err;

    const uint64_t coldHash = hashKey(kTier1Key);
    gm20b::Pipeline *cold = dev.pipeline(dev.pack().fastRun(coldHash), coldHash, gm20b::PackForm::Default, false);
    ASSERT_NE(cold, nullptr);
    EXPECT_FALSE(cold->methods.empty());
    EXPECT_EQ(cold->prebuiltWords, nullptr);
    EXPECT_GT(dev.heapInPlaceBytes(), 0u);
    EXPECT_FALSE(kernel_model::model.imports.empty());

    gm20b::RuntimeRecord rr;
    rr.pipeline.keyHash = 0xfeedfacecafebeefull;
    rr.pipeline.vs = 0;
    rr.pipeline.fs = 1;
    rr.keyString = "samples=1 c0=80:15:0:1:0:0:1:0:0 ";
    rr.blob = {0x1a00, 0x1111, 0x1364, 1, 0x1368, 2, 0x136c, 3, 0x12ec, 0};
    rr.programs[0].id = "rt_vs";
    rr.programs[0].image.assign(0x100, 0);
    rr.programs[0].cbufTypes = {gm20b::CbufRoot};
    rr.programs[0].gprs = 12;
    rr.programs[1].id = "rt_fs";
    rr.programs[1].image.assign(0x100, 0);
    rr.programs[1].cbufTypes = {gm20b::CbufRoot};
    rr.programs[1].gprs = 16;
    std::string why;
    gm20b::Pipeline *runtime = dev.addRecord(rr, false, why);
    ASSERT_NE(runtime, nullptr) << why;
    EXPECT_FALSE(runtime->methods.empty());
    EXPECT_EQ(runtime->prebuiltWords, nullptr);
}

TEST(GM20BPackHitStage3, Concurrent8ThreadFirstTier0CreationZeroLocksZeroKernelCallsTSanStress)
{
    const std::vector<uint8_t> file = readFileBytes(kFixturePath);
    ASSERT_FALSE(file.empty());

    constexpr size_t kThreads = 8;
    const std::array<const char *, 8> keys = {
        kPlainKey, kBlendInterlockKey, kNoPixelRast0Key, kTessKey,
        kFetchKey, kVariantsKey,       kStrideMainKey,   kPassWriteKey,
    };

    for (int round = 0; round < 3; ++round) {
        configurePackImageFromBytes(file, true);
        gm20b::Device dev;
        std::string err;
        ASSERT_TRUE(dev.open(kFixturePath, 0, err, gm20b::Device::Role::Client)) << err;
        dev.probeFetchRung.store(3, std::memory_order_relaxed);

        const uint64_t controlBefore = kernel_model::model.controlAllocs;
        const size_t bosBefore = kernel_model::model.bos.size();
        const size_t importsBefore = kernel_model::model.imports.size();
        const size_t bindsBefore = kernel_model::model.binds.size();

        std::array<std::array<gm20b::Pipeline *, keys.size()>, kThreads> got{};
        std::array<gm20b::Pipeline *, kThreads> gotVariantRec{};
        std::array<uint64_t, kThreads> threadLocks{};
        std::array<uint64_t, kThreads> threadKernelCalls{};
        std::atomic<uint32_t> ready{0};
        std::atomic<bool> go{false};

        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (size_t t = 0; t < kThreads; ++t) {
            workers.emplace_back([&, t] {
                gm20b::EncodeProfile profile;
                gm20b::tEncodeProfile = &profile;
                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();

                uint64_t locks0 = 0, waits0 = 0, ticks0 = 0;
                gm20b::LockCount::here(locks0, waits0, ticks0);

                // Every thread starts on the exact same state index first to maximize first-hit CAS contention.
                for (int iter = 0; iter < 8; ++iter) {
                    for (size_t k = 0; k < keys.size(); ++k) {
                        const uint64_t h = hashKey(keys[k]);
                        const gm20b::PackRun run = dev.pack().fastRun(h);
                        gm20b::Pipeline *pl = dev.pipeline(run, h, gm20b::PackForm::Default, false);
                        if (iter == 0)
                            got[t][k] = pl;
                        else
                            EXPECT_EQ(got[t][k], pl);
                        const auto vars = dev.variantPipelines(run, false);
                        if (k == 6 && !vars.empty()) {
                            if (iter == 0)
                                gotVariantRec[t] = vars[0].second;
                            else
                                EXPECT_EQ(gotVariantRec[t], vars[0].second);
                        }
                    }
                }

                uint64_t locks1 = 0, waits1 = 0, ticks1 = 0;
                gm20b::LockCount::here(locks1, waits1, ticks1);
                threadLocks[t] = locks1 - locks0;
                threadKernelCalls[t] = profile.calls[size_t(gm20b::EncodePart::Kernel)];
                gm20b::tEncodeProfile = nullptr;
            });
        }

        while (ready.load(std::memory_order_acquire) < kThreads)
            std::this_thread::yield();
        go.store(true, std::memory_order_release);
        for (std::thread &w : workers)
            w.join();

        for (size_t t = 0; t < kThreads; ++t) {
            EXPECT_EQ(threadLocks[t], 0u) << "round " << round << " thread " << t;
            EXPECT_EQ(threadKernelCalls[t], 0u) << "round " << round << " thread " << t;
        }
        EXPECT_EQ(kernel_model::model.controlAllocs, controlBefore);
        EXPECT_EQ(kernel_model::model.bos.size(), bosBefore);
        EXPECT_EQ(kernel_model::model.imports.size(), importsBefore);
        EXPECT_EQ(kernel_model::model.binds.size(), bindsBefore);
        EXPECT_EQ(dev.heapBytes(), 0u);
        EXPECT_EQ(dev.heapInPlaceBytes(), 0u);

        for (size_t k = 0; k < keys.size(); ++k) {
            ASSERT_NE(got[0][k], nullptr);
            for (size_t t = 1; t < kThreads; ++t)
                EXPECT_EQ(got[t][k], got[0][k]) << "key " << keys[k];
        }
        ASSERT_NE(gotVariantRec[0], nullptr);
        for (size_t t = 1; t < kThreads; ++t)
            EXPECT_EQ(gotVariantRec[t], gotVariantRec[0]);
        ASSERT_NE(__atomic_load_n(&got[0][4]->barrierTwin, __ATOMIC_ACQUIRE), nullptr);
        ASSERT_NE(__atomic_load_n(&got[0][4]->fetchFree, __ATOMIC_ACQUIRE), nullptr);
    }
}

}  // namespace
