// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../raw/runtime/raw_depth_stencil_state.h"
#include "../raw/runtime/raw_kit.h"
#include "../raw/runtime/raw_multisample.h"
#include "../raw/runtime/raw_owned.h"
#include "../raw/runtime/raw_pass_writes.h"
#include "../raw/runtime/raw_state_segment.h"
#include "../raw/runtime/raw_zcull.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include <cstring>
#include <map>
#include <vector>

namespace {

constexpr uint64_t kRingVa = 0x10'0000'0000, kHeapVa = 0x20'0000'0000;

// GPU memory as the GPFIFO reads it: each allocation by its VA
struct Memory {
    std::map<uint64_t, std::vector<uint8_t>> spans;

    uint8_t *allocate(uint64_t va, uint64_t bytes)
    {
        auto &span = spans[va];
        span.assign(bytes, 0xcd);
        return span.data();
    }

    std::vector<uint32_t> fetch(const raw::PushBatches &pushes) const
    {
        std::vector<uint32_t> words;
        for (size_t i = 0; i < pushes.size(); ++i) {
            const TegraGM20BAbiPush &p = pushes[i];
            auto it = spans.upper_bound(p.va);
            EXPECT_NE(it, spans.begin());
            --it;
            EXPECT_LE(p.va - it->first + uint64_t(p.dwords) * 4, it->second.size());
            const auto *at = reinterpret_cast<const uint32_t *>(it->second.data() + (p.va - it->first));
            words.insert(words.end(), at, at + p.dwords);
        }
        return words;
    }
};

raw::StateSegmentHeap heapOver(Memory &memory, uint64_t *grows = nullptr)
{
    return raw::StateSegmentHeap([&memory, grows](uint64_t bytes, raw::SegmentChunk &chunk) {
        const uint64_t va = kHeapVa + memory.spans.size() * 0x100000;
        chunk = {memory.allocate(va, bytes), va, bytes};
        if (grows)
            ++*grows;
        return true;
    });
}

// A state's stream as Device::build writes one: blend, write masks, programs and their register counts, a shader data
// constant buffer bound and the root table selected again
std::vector<uint32_t> stateWords(uint32_t salt)
{
    std::vector<uint32_t> words(256);
    raw::Push w{words.data(), words.data() + words.size()};
    w.m(NVB197_SET_BLEND(0), salt & 1);
    w.m(NVB197_SET_BLEND_SEPARATE_FOR_ALPHA, 1);
    w.m(NVB197_SET_CT_WRITE(0), 0x1111);
    w.m(NVB197_SET_PIXEL_SHADER_INTERLOCK_CONTROL, 0);
    w.m(NVB197_SET_HYBRID_ANTI_ALIAS_CONTROL, 1);
    w.m(NVB197_SET_PIPELINE_PROGRAM(1), 0x40000 + salt);
    w.m(NVB197_SET_PIPELINE_PROGRAM(5), 0x80000 + salt);
    w.m(NVB197_SET_PIPELINE_REGISTER_COUNT(1), 24);
    w.m(NVB197_SET_PIPELINE_REGISTER_COUNT(5), 32);
    w.inc(NVB197_SET_CONSTANT_BUFFER_SELECTOR_A, {0x100, 0x1, 0x2000 + salt});
    w.m(NVB197_BIND_GROUP_CONSTANT_BUFFER(4), 1 | 3 << 4);
    w.inc(NVB197_SET_CONSTANT_BUFFER_SELECTOR_A, {0x800, 0x1, 0x3000});
    words.resize(size_t(w.at - words.data()));
    return words;
}

// A pass body as RenderEncoder writes one around a bind: words before it, the state, a draw after it
struct Body {
    Memory &memory;
    uint32_t *ring;
    raw::Push p;
    std::vector<raw::PushBody> bodies;
    uint32_t *start;

    explicit Body(Memory &m) : memory(m), ring(reinterpret_cast<uint32_t *>(m.allocate(kRingVa, 0x10000)))
    {
        p = {ring, ring + 0x4000};
        start = ring;
    }

    void prefix(uint32_t tag)
    {
        p.m(NVB197_SET_PRIMITIVE_TOPOLOGY, 5);
        p.m(NVB197_NO_OPERATION, tag);
    }

    void draw(uint32_t first)
    {
        p.m(NVB197_BEGIN, 0);
        p.inc(NVB197_SET_VERTEX_ARRAY_START, {first, 4});
        p.m(NVB197_END, 0);
    }

    raw::PushBatches close()
    {
        if (p.at > start)
            bodies.push_back({start, p.at});
        raw::PushBatches pushes;
        raw::appendBodies(pushes, bodies, reinterpret_cast<const uint8_t *>(ring), kRingVa);
        return pushes;
    }
};

TEST(RawStateSegment, ABoundSegmentRunsTheWordsTheInlineStreamWrote)
{
    Memory memory;
    raw::StateSegmentHeap heap = heapOver(memory);
    const std::vector<uint32_t> a = stateWords(1), b = stateWords(2);
    const raw::StateSegment sa = heap.place(a.data(), uint32_t(a.size())),
                            sb = heap.place(b.data(), uint32_t(b.size()));
    ASSERT_TRUE(sa && sb);

    std::vector<uint32_t> inline_;
    {
        Memory scratch;
        Body body(scratch);
        body.prefix(1);
        body.p.raw(a.data(), uint32_t(a.size()));
        body.draw(0);
        body.draw(4);
        body.p.raw(b.data(), uint32_t(b.size()));
        body.draw(8);
        body.prefix(2);
        inline_ = scratch.fetch(body.close());
    }
    Body body(memory);
    body.prefix(1);
    raw::bindSegment(body.bodies, body.start, body.p.at, sa);
    body.draw(0);
    body.draw(4);
    raw::bindSegment(body.bodies, body.start, body.p.at, sb);
    body.draw(8);
    body.prefix(2);
    const raw::PushBatches pushes = body.close();
    EXPECT_EQ(memory.fetch(pushes), inline_);
    // the ring stretches, each segment its own entry, prefetched
    ASSERT_EQ(pushes.size(), 5u);
    EXPECT_EQ(pushes[1].va, sa.va);
    EXPECT_EQ(pushes[3].va, sb.va);
    EXPECT_EQ(pushes[1].flags | pushes[3].flags, 0u);
    // the ring words the CPU wrote: everything but the states
    EXPECT_EQ(uint64_t(body.p.at - body.ring), inline_.size() - a.size() - b.size());
}

TEST(RawStateSegment, BodiesKeepIndirectArgumentsUnprefetchedAndBatchCuts)
{
    Memory memory;
    raw::StateSegmentHeap heap = heapOver(memory);
    const std::vector<uint32_t> a = stateWords(3);
    const raw::StateSegment sa = heap.place(a.data(), uint32_t(a.size()));
    Body body(memory);
    raw::bindSegment(body.bodies, body.start, body.p.at, sa);
    body.p.m(NVB197_NO_OPERATION, 7);
    body.bodies.push_back({body.start, body.p.at, 0, 0, true});
    const uint64_t arguments = memory.spans.rbegin()->first + 0x8000;
    body.bodies.push_back(raw::PushBody::resident(arguments, 4, TegraGM20BPushNoPrefetch));
    body.bodies.back().batchEnd = true;
    body.start = body.p.at;
    body.draw(0);
    const raw::PushBatches pushes = body.close();
    ASSERT_EQ(pushes.size(), 4u);
    EXPECT_EQ(pushes[0].va, sa.va);
    EXPECT_EQ(pushes[1].flags, uint32_t(TegraGM20BPushIncomplete));
    EXPECT_EQ(pushes[2].flags, uint32_t(TegraGM20BPushNoPrefetch));
    EXPECT_EQ(pushes.next(0, 255), 3u);  // the cut after the indirect arguments, never inside the call
}

// gpu_exec.c's and TegraGM20BUserClientJobSubmit.cpp's refusal: an incomplete push the job's last or before a prefetched one
bool kernelTakes(const raw::PushBatches &pushes)
{
    for (size_t i = 0; i < pushes.size(); ++i)
        if ((pushes[i].flags & TegraGM20BPushIncomplete) &&
            (i + 1 == pushes.size() || !(pushes[i + 1].flags & TegraGM20BPushNoPrefetch)))
            return false;
    return true;
}

TEST(RawStateSegment, AColourMatrixCallReadsItsUniformUnprefetchedAndItsClampPairAfterTheRows)
{
    for (const uint32_t words : {11u, 10u}) {
        Memory memory;
        Body body(memory);
        body.p.m(NVB197_NO_OPERATION, 3);
        const uint64_t uniform = 0x30'0000'0000, clamp = 0x40'0000'0000;
        uint32_t *rows = reinterpret_cast<uint32_t *>(memory.allocate(uniform, 4 * words));
        for (uint32_t k = 0; k < words; ++k)
            rows[k] = 0x3c00'0000 | k;
        memset(memory.allocate(clamp, 4), 0, 4);
        raw::matrixCall(body.bodies, body.start, body.p.at, uniform, words, clamp, 11 - words);
        body.start = body.p.at;
        body.draw(0);
        const raw::PushBatches pushes = body.close();
        ASSERT_EQ(pushes.size(), words == 11 ? 3u : 4u) << words;
        EXPECT_TRUE(kernelTakes(pushes)) << words;
        EXPECT_EQ(pushes[1].va, uniform);
        EXPECT_EQ(pushes[1].dwords, words);
        EXPECT_TRUE(pushes[1].flags & TegraGM20BPushNoPrefetch);
        const std::vector<uint32_t> fetched = memory.fetch(pushes);
        const size_t at = size_t(pushes[0].dwords);
        for (uint32_t k = 0; k < words; ++k)
            EXPECT_EQ(fetched[at + k], rows[k]);
        if (words == 10) {
            EXPECT_EQ(pushes[2].va, clamp);
            EXPECT_EQ(pushes[2].flags, uint32_t(TegraGM20BPushNoPrefetch));
            EXPECT_EQ(fetched[at + 10], 0u);
        }
    }
}

TEST(RawStateSegment, TheHeapCopiesEachStreamOnceAndNeverMovesIt)
{
    Memory memory;
    uint64_t grows = 0;
    raw::StateSegmentHeap heap = heapOver(memory, &grows);
    std::vector<raw::StateSegment> placed;
    std::vector<std::vector<uint32_t>> streams;
    for (uint32_t i = 0; i < 1200; ++i) {
        streams.push_back(stateWords(i));
        placed.push_back(heap.place(streams.back().data(), uint32_t(streams.back().size())));
        ASSERT_TRUE(placed.back());
        EXPECT_EQ(placed.back().va % 16, 0u);
    }
    EXPECT_GT(grows, 1u);
    for (size_t i = 0; i < placed.size(); ++i) {
        raw::PushBatches one;
        one.append({placed[i].va, placed[i].words, 0}, false);
        EXPECT_EQ(memory.fetch(one), streams[i]) << i;
    }
}

TEST(RawStateSegment, AStreamPlacedAgainIsTheSegmentAlreadyPlaced)
{
    Memory memory;
    raw::StateSegmentHeap heap = heapOver(memory);
    const std::vector<uint32_t> a = stateWords(5), b = stateWords(6);
    const raw::StateSegment first = heap.place(a.data(), uint32_t(a.size()));
    const uint64_t bytes = heap.bytesPlaced();
    EXPECT_EQ(heap.place(a.data(), uint32_t(a.size())).va, first.va);
    EXPECT_EQ(heap.bytesPlaced(), bytes);
    EXPECT_NE(heap.place(b.data(), uint32_t(b.size())).va, first.va);
    raw::DepthStencilState one, another;
    one.place(heap);
    const uint64_t states = heap.bytesPlaced();
    another.place(heap);
    EXPECT_EQ(heap.bytesPlaced(), states);
    EXPECT_EQ(another.segment(true, true).va, one.segment(true, true).va);
}

TEST(RawStateSegment, ShortStreamsAndAHeapWithoutMemoryStayInline)
{
    Memory memory;
    raw::StateSegmentHeap heap = heapOver(memory);
    const uint32_t few[raw::kSegmentBindWords] = {};
    EXPECT_FALSE(heap.place(few, raw::kSegmentBindWords));
    raw::StateSegmentHeap empty([](uint64_t, raw::SegmentChunk &) {
        return false;
    });
    const std::vector<uint32_t> a = stateWords(0);
    EXPECT_FALSE(empty.place(a.data(), uint32_t(a.size())));
    EXPECT_EQ(empty.bytesPlaced(), 0u);
}

TEST(RawStateSegment, AChannelHoldsTheLastSegmentUntilSomethingElseWritesItsRegisters)
{
    Memory memory;
    raw::StateSegmentHeap heap = heapOver(memory);
    const std::vector<uint32_t> kept = stateWords(0);
    std::vector<uint32_t> culled = kept;
    culled.push_back(raw::Push::hdr(4, raw::SUBC_3D, NVB197_OGL_SET_CULL, 1));
    const raw::StateSegment a = heap.place(kept.data(), uint32_t(kept.size())),
                            b = heap.place(culled.data(), uint32_t(culled.size()));
    ASSERT_TRUE(a.keptAcrossPasses);
    ASSERT_FALSE(b.keptAcrossPasses);
    raw::HeldSegment held;
    EXPECT_FALSE(held.holds(a));
    held.bound(a);
    EXPECT_TRUE(held.holds(a));
    EXPECT_FALSE(held.holds(b));
    EXPECT_FALSE(held.holds(raw::StateSegment()));
    held.passBoundary();
    EXPECT_TRUE(held.holds(a));
    held.bound(b);
    held.passBoundary();
    EXPECT_FALSE(held.holds(b));
    held.bound(a);
    held.lose();
    EXPECT_FALSE(held.holds(a));
}

TEST(RawStateSegment, AStreamThatCannotBeReadIsNeverKeptAcrossPasses)
{
    const uint32_t cut[2] = {raw::Push::hdr(1, raw::SUBC_3D, NVB197_SET_BLEND(0), 4), 0};
    EXPECT_FALSE(raw::keptAcrossPasses(cut, 2));
    const uint32_t compute[2] = {raw::Push::hdr(4, raw::SUBC_COMPUTE, 0x0200, 1), 0};
    EXPECT_FALSE(raw::keptAcrossPasses(compute, 1));
}

// The registers each draw (BEGIN) of a run of methods sees
std::vector<std::map<uint32_t, uint32_t>> registersAtDraws(const std::vector<uint32_t> &words)
{
    std::vector<switch_gm20b_method> methods(words.size());
    const long n = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
    EXPECT_GE(n, 0);
    std::map<uint32_t, uint32_t> registers;
    std::vector<std::map<uint32_t, uint32_t>> draws;
    for (long i = 0; i < n; ++i) {
        registers[methods[size_t(i)].method] = methods[size_t(i)].value;
        if (methods[size_t(i)].method == NVB197_BEGIN)
            draws.push_back(registers);
    }
    return draws;
}

TEST(RawStateSegment, ADepthStencilBindLeavesTheRegistersTheDiffDid)
{
    raw::DepthStencilDesc stencilled;
    stencilled.depthCompare = 1;
    stencilled.depthWrite = true;
    stencilled.front = {3, 1, 2, 7, 0xf0, 0x0f};
    stencilled.back = {5, 3, 4, 6, 0x3c, 0xc3};
    const raw::DepthStencilState plain, tested(stencilled);
    for (const bool depth : {false, true})
        for (const bool stencil : {false, true}) {
            Memory memory;
            raw::StateSegmentHeap heap = heapOver(memory);
            raw::DepthStencilState a = plain, b = tested;
            a.place(heap);
            b.place(heap);
            ASSERT_TRUE(b.segment(depth, stencil));
            raw::DepthStencilRegs inlineHeld, segmentHeld;
            for (uint32_t &v : inlineHeld.v)
                v = ~0u;
            segmentHeld = inlineHeld;
            Memory scratch;
            Body direct(scratch), body(memory);
            uint32_t binds = 0;
            for (const auto &[state, refs] : {std::pair{&a, 0u}, std::pair{&b, 0x11u}, std::pair{&b, 0x22u},
                                              std::pair{&a, 0x22u}, std::pair{&b, 0x22u}}) {
                const raw::DepthStencilRegs want = state->regs(depth, stencil, refs, refs + 1);
                raw::emitDepthStencil(direct.p, want, inlineHeld);
                binds += raw::bindDepthStencil(body.p, body.bodies, body.start, want, segmentHeld,
                                               state->segment(depth, stencil));
                EXPECT_EQ(memcmp(segmentHeld.v, inlineHeld.v, sizeof inlineHeld.v), 0);
                direct.draw(0);
                body.draw(0);
            }
            const std::vector<uint32_t> before = scratch.fetch(direct.close()), after = memory.fetch(body.close());
            EXPECT_EQ(registersAtDraws(after), registersAtDraws(before));
            EXPECT_GE(binds, 1u) << depth << stencil;
            EXPECT_LT(uint64_t(body.p.at - body.ring), uint64_t(direct.p.at - direct.ring)) << depth << stencil;
        }
}

// What a pass boundary writes (its epilogue's raster state and base registers, the next prologue's barrier, swizzle,
// sample layout, targets, clears and zcull region) is owned, an action, or in kPassWrites
std::vector<uint32_t> passBoundaryWords()
{
    std::vector<uint32_t> words(4096);
    raw::Push p{words.data(), words.data() + words.size()};
    raw::kit(p, switch_gm20b_pass_barrier, true);
    raw::kit(p, switch_gm20b_viewport_swizzle, 0u, 0x3210u);
    raw::SampleLayout layout;
    raw::sampleLayout(4, layout);
    raw::sampleLayoutMethods(p, layout, 0x800, 0x1000);
    switch_gm20b_target target = {};
    target.address = 0x100000;
    target.width = 64;
    target.height = 64;
    target.layers = 1;
    for (uint32_t i = 0; i < 8; ++i)
        raw::kit(p, switch_gm20b_color_target, i, &target, true);
    raw::kit(p, switch_gm20b_render_area, 2u, 64u, 64u);
    switch_gm20b_clear_state clear = {};
    clear.xmax = clear.ymax = 64;
    clear.depth = clear.stencil = true;
    raw::kit(p, switch_gm20b_clear, &clear);
    raw::ZcullRegion region;
    region.width = region.height = 64;
    region.subregionCount = 2;
    raw::zcullRegionWords(p, region, 252, raw::kZcullGreater);
    raw::zcullOffWords(p);
    for (uint32_t m : {NVB197_SET_POLY_OFFSET_POINT,
                       NVB197_SET_POLY_OFFSET_LINE,
                       NVB197_SET_POLY_OFFSET_FILL,
                       NVB197_SET_DEPTH_BIAS,
                       NVB197_SET_SLOPE_SCALE_DEPTH_BIAS,
                       NVB197_SET_DEPTH_BIAS_CLAMP,
                       NVB197_OGL_SET_CULL,
                       NVB197_OGL_SET_CULL_FACE,
                       NVB197_OGL_SET_FRONT_FACE,
                       NVB197_SET_FRONT_POLYGON_MODE,
                       NVB197_SET_BACK_POLYGON_MODE,
                       NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX,
                       NVB197_SET_GLOBAL_BASE_VERTEX_INDEX,
                       NVB197_SET_VERTEX_ID_BASE,
                       NVB197_SET_ZT_SELECT,
                       NVB197_SET_ZT_LAYER,
                       NVB197_SET_Z_COMPRESSION,
                       NVB197_SET_STENCIL_COMPRESSION,
                       NVB197_SET_ZT_SPARSE,
                       NVB197_SET_Z_CLEAR_VALUE,
                       NVB197_SET_STENCIL_CLEAR_VALUE})
        p.m(m, 0);
    words.resize(size_t(p.at - words.data()));
    return words;
}

bool action(uint32_t m)
{
    switch (m) {
    case NVB197_WAIT_FOR_IDLE:
    case NVB197_INVALIDATE_SHADER_CACHES:
    case NVB197_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI:
    case NVB197_CLEAR_SURFACE:
    case SWITCH_NVB197_TILED_CACHE_FLUSH:
    case 0x02fc:  // ASSIGN_ZCULL_SUBREGIONS
        return true;
    default:
        return false;
    }
}

TEST(RawStateSegment, EveryRegisterAPassBoundaryWritesIsOwnedOrAPassWrite)
{
    const std::vector<uint32_t> words = passBoundaryWords();
    std::vector<switch_gm20b_method> methods(words.size());
    const long n = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
    ASSERT_GT(n, 0);
    for (long i = 0; i < n; ++i) {
        const switch_gm20b_method &m = methods[size_t(i)];
        if (m.subchannel != 0 || action(m.method))
            continue;
        EXPECT_TRUE(raw::ownedMethod(m.method) || raw::passWrites(m.method))
            << std::hex << "method 0x" << m.method << " is written at a pass boundary but a state segment may hold it";
    }
}

}  // namespace
