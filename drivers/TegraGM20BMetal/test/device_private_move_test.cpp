// SPDX-License-Identifier: MIT
// private_comp.cpp on gm20b::Device over the kernel model: a private target made plain because no
// compressed memory of its size was ready at its creation moves onto that memory once it is made (Device::wantCompressed,
// privateMoves, compressedFromPlain), as the submit worker moves it at its next raw range, and a look is due only where
// memory that serves a want came.
#include "device_private_comp.h"
#include <array>
#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

namespace {

using private_comp::copies;
using private_comp::DevicePrivateComp;
using private_comp::kBGRA8;
using private_comp::kH;
using private_comp::kW;

std::vector<void *> taken;

void take(void *owner)
{
    taken.push_back(owner);
}

// The owners the worker's look takes
std::vector<void *> look(gm20b::Device &d)
{
    taken.clear();
    d.privateMoves(take);
    return taken;
}

// Core Animation's zoom window target made plain for want of compressed memory: once some is made, the worker moves it,
// its texels by the copy engine in a job of its own that then rewrites its header and the one viewTic made from it in
// place
TEST_F(DevicePrivateComp, APlainTargetMovesOnceItsMemoryIsMade)
{
    gm20b::Texture t, other;
    zoomTarget(t);
    ASSERT_FALSE(t.comptags) << "none ready at its creation: asked for";
    awaitMade();
    EXPECT_TRUE(dev.privateMoveDue()) << "memory made that serves a want";
    ASSERT_TRUE(dev.createTexture(64, 64, kBGRA8, other, false, true));
    EXPECT_EQ(look(dev), std::vector<void *>{&t});
    EXPECT_FALSE(dev.privateMoveDue()) << "the look clears it";
    const uint32_t derived = dev.viewTic(t, gm20b::formatInfo(81)->tic0);  // BGRA8Unorm_sRGB
    ASSERT_NE(derived, 0u);
    commitEmpty();
    const uint64_t plainVa = t.va;
    const uint32_t tic = t.tic;
    const std::array<uint32_t, 8> own = gm20b::DeviceHarness::header(dev, tic),
                                  view = gm20b::DeviceHarness::header(dev, derived);
    const size_t jobs = kernel_model::model.jobs.size();
    ASSERT_TRUE(dev.compressedFromPlain(t));
    EXPECT_TRUE(t.comptags);
    EXPECT_NE(t.va, plainVa);
    EXPECT_EQ(t.tic, tic) << "a gpuResourceID handed out names the same header";
    EXPECT_EQ(kernel_model::model.jobs.size(), jobs + 1) << "the move is a job of its own";
    const auto launched = copies(jobs);
    ASSERT_FALSE(launched.empty());
    EXPECT_EQ(launched.front(), (std::pair<uint64_t, uint64_t>{plainVa, t.va})) << "its texels first";
    for (const auto &[header, before] : {std::make_pair(tic, own), std::make_pair(derived, view)}) {
        uint32_t want[8];
        memcpy(want, before.data(), 32);
        gm20b::ticAt(want, t.va);
        const uint64_t headerVa = gm20b::DeviceHarness::ticVa(dev, header);
        const auto into = std::find_if(launched.begin(), launched.end(), [&](const auto &l) {
            return l.second == headerVa;
        });
        ASSERT_NE(into, launched.end()) << "header " << header << " rewritten by the move's job";
        EXPECT_EQ(memcmp(kernel_model::model.host(into->first), want, 32), 0) << "header " << header;
    }
    EXPECT_EQ(dev.stats.privateCompressedMoves.load(), 1u);
    EXPECT_TRUE(dev.privateCompressed(t));
    EXPECT_EQ(gm20b::DeviceHarness::wanting(dev), 0u);
    EXPECT_FALSE(dev.compressedFromPlain(t)) << "once";
    EXPECT_EQ(gm20b::DeviceHarness::movedPlain(dev), 1u);
    commitEmpty();
    EXPECT_EQ(gm20b::DeviceHarness::movedPlain(dev), 0u) << "the plain memory went once the move's job completed";
    dev.destroyTexture(other);
    dev.destroyTexture(t);
}

// Every maker of a header over a target's memory, around a move. One made before it keeps its index, and the move's job
// rewrites it: the same key asked again answers that index (BGRA8Unorm and BGRA8Unorm_sRGB share word 0, sRGB being a
// header bit, so their views ask one key). One made after it, before that job ran, names the compressed memory. The
// model runs no job, so its table keeps a header the job rewrites as it was; the job's launches show the rewrite.
TEST_F(DevicePrivateComp, EveryHeaderOverAMovedTargetNamesItsNewMemory)
{
    gm20b::Texture t, view, part;
    zoomTarget(t);
    awaitMade();
    const uint32_t bgra8 = gm20b::formatInfo(kBGRA8)->tic0;
    gm20b::AttachmentView own;
    own.word0 = bgra8;
    own.layers = 1;
    const char *why = nullptr;
    const uint32_t before[] = {dev.viewTic(t, bgra8), dev.attachmentViewTic(t, own, &why),
                               dev.attachmentTic(t, t, 0, 0)};
    std::vector<std::array<uint32_t, 8>> words;
    for (const uint32_t tic : before) {
        ASSERT_NE(tic, 0u) << (why ? why : "");
        words.push_back(gm20b::DeviceHarness::header(dev, tic));
    }
    commitEmpty();
    const uint64_t plainVa = t.va;
    const size_t jobs = kernel_model::model.jobs.size();
    ASSERT_TRUE(dev.compressedFromPlain(t));
    const auto launched = copies(jobs);
    for (size_t i = 0; i < words.size(); i++) {
        EXPECT_EQ(gm20b::ticAddress(words[i].data()), plainVa) << "maker " << i;
        uint32_t want[8];
        memcpy(want, words[i].data(), 32);
        gm20b::ticAt(want, t.va);
        const uint64_t headerVa = gm20b::DeviceHarness::ticVa(dev, before[i]);
        const auto into = std::find_if(launched.begin(), launched.end(), [&](const auto &l) {
            return l.second == headerVa;
        });
        ASSERT_NE(into, launched.end()) << "maker " << i << ": rewritten by the move's job";
        EXPECT_EQ(memcmp(kernel_model::model.host(into->first), want, 32), 0) << "maker " << i;
    }
    EXPECT_EQ(dev.viewTic(t, gm20b::formatInfo(81)->tic0), before[0]);
    EXPECT_EQ(dev.attachmentViewTic(t, own, &why), before[1]);
    EXPECT_EQ(dev.attachmentTic(t, t, 0, 0), before[2]);
    gm20b::AttachmentView other = own;
    other.word0 = gm20b::formatInfo(53)->tic0;  // R32Uint
    gm20b::Texture srgb = t;
    srgb.format = 81;
    ASSERT_TRUE(dev.createView(t, 81, view));
    ASSERT_TRUE(dev.createPartView(t, kBGRA8, gm20b::fx::kTexTwoD, 0, 1, 0, 1, part));
    const std::pair<const char *, uint32_t> after[] = {
        {"viewTic", dev.viewTic(t, gm20b::formatInfo(70)->tic0)},  // RGBA8Unorm
        {"attachmentViewTic", dev.attachmentViewTic(t, other, &why)},
        {"attachmentTic", dev.attachmentTic(t, srgb, 0, 0)},
        {"createView", view.tic},
        {"createPartView", part.tic},
    };
    for (const auto &[maker, tic] : after) {
        ASSERT_NE(tic, 0u) << maker;
        EXPECT_EQ(gm20b::ticAddress(gm20b::DeviceHarness::header(dev, tic).data()), t.va)
            << maker << ": made after the move, before its job ran";
    }
    EXPECT_TRUE(view.comptags);
    EXPECT_EQ(part.va, t.va);
    commitEmpty();
    dev.destroyTexture(part);
    dev.destroyTexture(view);
    dev.destroyTexture(t);
}

// A look clears the flag whatever it found; no count of command buffers or time sets it again, memory kept that serves
// the want does
TEST_F(DevicePrivateComp, ALookIsDueOnlyAfterMemoryThatServesAWantComes)
{
    EXPECT_FALSE(dev.privateMoveDue()) << "nothing wants compressed memory";
    gm20b::Texture t, kept;
    zoomTarget(t);
    awaitMade();
    ASSERT_TRUE(dev.privateMoveDue());
    EXPECT_EQ(look(dev).size(), 1u);
    for (int i = 0; i < 64; i++)
        commitEmpty();
    EXPECT_FALSE(dev.privateMoveDue());
    ASSERT_TRUE(dev.createCompressedTexture(kW, kH, kBGRA8, gm20b::kCompKindC32, kept)) << "the memory made";
    EXPECT_FALSE(dev.privateMoveDue());
    dev.destroyTexture(kept);
    EXPECT_TRUE(dev.privateMoveDue()) << "a closed target's memory kept that serves the want";
    dev.destroyTexture(t);
}

// A want that no ready memory serves (another texture took its memory; what comes next is of another kind) gets no look:
// nothing runs on the worker's path for it until memory that serves it comes
TEST_F(DevicePrivateComp, MemoryThatServesNoWantBringsNoLook)
{
    gm20b::Texture t, taker, other;
    zoomTarget(t);
    awaitMade();
    EXPECT_EQ(look(dev).size(), 1u);
    ASSERT_TRUE(dev.createCompressedTexture(kW, kH, kBGRA8, gm20b::kCompKindC32, taker)) << "its memory taken";
    ASSERT_FALSE(dev.createCompressedTexture(256, 256, 115, gm20b::kCompKindC64, other)) << "RGBA16Float: asked";
    awaitMade();
    EXPECT_FALSE(dev.privateMoveDue()) << "C64 memory serves no C32 want";
    EXPECT_TRUE(look(dev).empty());
    dev.destroyTexture(taker);
    EXPECT_TRUE(dev.privateMoveDue()) << "its kind and size kept";
    dev.destroyTexture(t);
}

// A raw view's header names the plain memory, and the view may go, its header index another texture's, before a move's
// job runs: a target with one stays plain for good
TEST_F(DevicePrivateComp, APlainTargetWithARawViewStaysPlain)
{
    gm20b::Texture t, view;
    zoomTarget(t);
    ASSERT_TRUE(dev.createView(t, 81, view));
    awaitMade();
    EXPECT_TRUE(look(dev).empty());
    EXPECT_EQ(gm20b::DeviceHarness::wanting(dev), 0u);
    const size_t jobs = kernel_model::model.jobs.size();
    EXPECT_FALSE(dev.compressedFromPlain(t));
    EXPECT_FALSE(t.comptags);
    EXPECT_EQ(kernel_model::model.jobs.size(), jobs);
    dev.destroyTexture(view);
    dev.destroyTexture(t);
}

// The texels a move copied from `plain` (launches since job `jobs`): each of `layers` layers, `stride` apart
void expectLayersMoved(size_t jobs, uint64_t plain, uint64_t to, uint64_t stride, uint32_t layers)
{
    const auto launched = copies(jobs);
    for (uint32_t layer = 0; layer < layers; layer++)
        EXPECT_NE(
            std::find(launched.begin(), launched.end(), std::make_pair(plain + layer * stride, to + layer * stride)),
            launched.end())
            << "layer " << layer;
}

// A stored one-sample Depth32Float texture made plain for want of ZF32_2CZ memory moves onto it once it is made, as a
// colour target does: its zeta memory copied by the copy engine, its header rewritten in place, its plain zeta memory
// given back once the job completed and its compressed memory kept for the next of its kind when it goes
TEST_F(DevicePrivateComp, AStoredDepthTextureMadePlainMovesOntoCompressedMemory)
{
    gm20b::Texture z;
    ASSERT_TRUE(dev.createOwnedZeta(96, 80, 252, 1, true, z));
    ASSERT_FALSE(z.comptags) << "none ready at its creation: asked for";
    ASSERT_TRUE(dev.wantCompressed(z, gm20b::kCompKindZF32, &z));
    awaitMade();
    EXPECT_TRUE(dev.privateMoveDue());
    EXPECT_EQ(look(dev), std::vector<void *>{&z});
    commitEmpty();
    const uint64_t plainVa = z.va;
    const uint32_t tic = z.tic;
    const size_t jobs = kernel_model::model.jobs.size();
    ASSERT_TRUE(dev.compressedFromPlain(z));
    EXPECT_TRUE(z.comptags);
    EXPECT_TRUE(z.zeta);
    EXPECT_EQ(z.tic, tic);
    EXPECT_TRUE(bound(gm20b::kCompKindZF32));
    expectLayersMoved(jobs, plainVa, z.va, 0, 1);
    EXPECT_EQ(z.storageRange().va, z.va);
    EXPECT_EQ(gm20b::DeviceHarness::movedPlain(dev), 1u);
    commitEmpty();
    EXPECT_EQ(gm20b::DeviceHarness::movedPlain(dev), 0u) << "the plain zeta memory went once the move's job completed";
    const size_t freed = kernel_model::model.freed.size(), ready = gm20b::DeviceHarness::compReady(dev);
    dev.destroyTexture(z);
    EXPECT_EQ(kernel_model::model.freed.size(), freed);
    EXPECT_EQ(gm20b::DeviceHarness::compReady(dev), ready + 1);
}

// A one-level array made plain wants memory for every layer, and its move copies each
TEST_F(DevicePrivateComp, AnArrayMadePlainMovesEveryLayer)
{
    gm20b::Texture t;
    ASSERT_TRUE(dev.createArray(128, 128, 3, kBGRA8, gm20b::fx::kTexTwoDArray, t, 1, nullptr, gm20b::kCompKindC32));
    ASSERT_FALSE(t.comptags);
    ASSERT_TRUE(dev.wantCompressed(t, gm20b::kCompKindC32, &t));
    awaitMade();
    EXPECT_EQ(look(dev), std::vector<void *>{&t});
    commitEmpty();
    const uint64_t plainVa = t.va, stride = t.layerBytes;
    const size_t jobs = kernel_model::model.jobs.size();
    ASSERT_TRUE(dev.compressedFromPlain(t));
    EXPECT_TRUE(t.comptags);
    EXPECT_GE(t.storage.bytes, 3 * stride) << "its storage names every layer";
    expectLayersMoved(jobs, plainVa, t.va, stride, 3);
    commitEmpty();
    dev.destroyTexture(t);
}

// A stored multisampled target made plain moves as the grid of its samples
TEST_F(DevicePrivateComp, AStoredMultisampledTargetMadePlainMovesItsSampleGrid)
{
    gm20b::Texture t;
    const uint32_t kind = gm20b::compressedKind(4, 0, 4);
    ASSERT_TRUE(dev.createStoredMultisample(64, 64, kBGRA8, 4, t, nullptr, kind));
    ASSERT_FALSE(t.comptags);
    ASSERT_TRUE(dev.wantCompressed(t, kind, &t));
    awaitMade();
    EXPECT_EQ(look(dev), std::vector<void *>{&t});
    commitEmpty();
    const uint64_t plainVa = t.va;
    const size_t jobs = kernel_model::model.jobs.size();
    ASSERT_TRUE(dev.compressedFromPlain(t));
    EXPECT_TRUE(t.comptags);
    EXPECT_EQ(t.samples, 4u);
    EXPECT_TRUE(bound(kind)) << "C32_MS4_2CRA";
    expectLayersMoved(jobs, plainVa, t.va, 0, 1);
    commitEmpty();
    dev.destroyTexture(t);
}

// A mip chain stays plain: no move rewrites its level headers
TEST_F(DevicePrivateComp, AChainMadePlainWantsNoMemory)
{
    gm20b::Texture t;
    ASSERT_TRUE(dev.createMipTexture(256, 256, kBGRA8, 4, t, nullptr, gm20b::kCompKindC32));
    ASSERT_FALSE(t.comptags);
    EXPECT_FALSE(dev.wantCompressed(t, gm20b::kCompKindC32, &t));
    dev.destroyTexture(t);
}

// A test that ends with an ask in flight, its kernel call held open until the fixture's own members are gone, then
// let go on to bind as the device's teardown begins: that bind reaches none of them
TEST_F(DevicePrivateComp, ATestEndingWithAnAskInFlightLeavesTheDeviceNothingGoneToReach)
{
    held.arm();
    gm20b::Texture t;
    ASSERT_FALSE(dev.createCompressedTexture(512, 512, kBGRA8, gm20b::kCompKindC32, t)) << "none made yet: asked";
    held.awaitHeld();
}

// A want whose ask the kernel refused for a shortage asks again when this process gives compressed memory back (the
// only event that changes the kernel's answer), and the memory then made serves it: no later texture of its size is
// needed
TEST_F(DevicePrivateComp, AWantRefusedForAShortageAsksAgainWhenMemoryIsGivenBack)
{
    gm20b::Texture t, asked, first, other;
    // made plain before the refusal is armed: the next BoAlloc of any kind takes it, the compressed ask's alone here
    ASSERT_TRUE(dev.createTexture(kW, kH, kBGRA8, t, false, true));
    const uint64_t failed = dev.stats.compFailed.load();
    kernel_model::model.refuseAllocCount = 1;
    ASSERT_FALSE(dev.createCompressedTexture(kW, kH, kBGRA8, gm20b::kCompKindC32, asked)) << "none made yet: asked";
    settle();
    ASSERT_GT(dev.stats.compFailed.load(), failed) << "its ask was refused";
    ASSERT_TRUE(dev.wantCompressed(t, gm20b::kCompKindC32, &t));
    compressedSecond(first, other, [&](gm20b::Texture &x) {
        return dev.createCompressedTexture(256, 256, kBGRA8, gm20b::kCompKindC32, x) ||
               dev.createTexture(256, 256, kBGRA8, x, false, true);
    });
    EXPECT_FALSE(dev.privateMoveDue());
    dev.destroyTexture(other);
    awaitMade();
    EXPECT_TRUE(dev.privateMoveDue()) << "the zoom target's want asked again and made";
    EXPECT_EQ(look(dev), std::vector<void *>{&t});
    dev.destroyTexture(first);
    dev.destroyTexture(t);
}

}  // namespace
