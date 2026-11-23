// SPDX-License-Identifier: MIT
// private_comp.cpp on gm20b::Device over the kernel model: private chains, arrays and stored multisampled
// textures on compressed memory made off the render thread (TextureAsk::compressKind). Then the clears of a pass over
// Core Animation's zoom window target (boot 21: 1792x1472 BGRA8) whose memoryless second colour only its draws see
// (clear_box.h).
#include "device_private_comp.h"
#include "../record.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

namespace {

using private_comp::DevicePrivateComp;
using private_comp::kBGRA8;
using private_comp::kH;
using private_comp::kW;

constexpr uint32_t kRGBA16F = 115, kTriangle = 3;

TEST_F(DevicePrivateComp, AChainTakesCompressedMemoryForEveryLevel)
{
    gm20b::Texture first, chain;
    compressedSecond(first, chain, [&](gm20b::Texture &t) {
        return dev.createMipTexture(256, 256, kBGRA8, 5, t, nullptr, gm20b::kCompKindC32);
    });
    EXPECT_TRUE(bound(gm20b::kCompKindC32));
    ASSERT_TRUE(chain.chain);
    EXPECT_EQ(chain.chain->va, chain.va);
    EXPECT_EQ(chain.cpu, nullptr) << "no CPU view of compressed memory";
    gm20b::Texture level;
    ASSERT_TRUE(gm20b::chainLevelImage(chain, 1, level));
    EXPECT_TRUE(level.comptags) << "level 1 is rendered with SET_COLOR_COMPRESSION, as level 0 is";
    ASSERT_TRUE(gm20b::chainLevelTarget(chain, 2, level));
    EXPECT_TRUE(level.comptags);
    dev.destroyTexture(chain);
    dev.destroyTexture(first);
}

TEST_F(DevicePrivateComp, AnArrayTakesCompressedMemoryForEveryLayer)
{
    gm20b::Texture first, array;
    compressedSecond(first, array, [&](gm20b::Texture &t) {
        return dev.createArray(128, 128, 3, kBGRA8, gm20b::fx::kTexTwoDArray, t, 1, nullptr, gm20b::kCompKindC32);
    });
    ASSERT_EQ(array.layers, 3u);
    EXPECT_GE(array.storage.bytes, 3 * array.layerBytes);
    dev.destroyTexture(array);
    dev.destroyTexture(first);
}

TEST_F(DevicePrivateComp, AStoredMultisampledTextureTakesItsSampleCountsKind)
{
    gm20b::Texture first, ms;
    const uint32_t kind = gm20b::compressedKind(4, 0, 4);
    compressedSecond(first, ms, [&](gm20b::Texture &t) {
        return dev.createStoredMultisample(64, 64, kBGRA8, 4, t, nullptr, kind);
    });
    EXPECT_TRUE(bound(kind)) << "C32_MS4_2CRA";
    EXPECT_EQ(ms.samples, 4u);
    dev.destroyTexture(ms);
    dev.destroyTexture(first);
}

TEST_F(DevicePrivateComp, ACompressedTexturesMemoryStaysMadeWhenItGoes)
{
    gm20b::Texture first, t;
    compressedSecond(first, t, [&](gm20b::Texture &x) {
        return dev.createMipTexture(128, 128, kBGRA8, 3, x, nullptr, gm20b::kCompKindC32);
    });
    const size_t freedBefore = kernel_model::model.freed.size(), ready = gm20b::DeviceHarness::compReady(dev);
    dev.destroyTexture(t);
    EXPECT_EQ(kernel_model::model.freed.size(), freedBefore);
    EXPECT_EQ(gm20b::DeviceHarness::compReady(dev), ready + 1) << "made for the next texture of its kind";
    dev.destroyTexture(first);
}

// Core Animation's zoom window target, made again at each app open and close: the memory a closed one held makes the
// next one compressed at its creation, with no BoAlloc
TEST_F(DevicePrivateComp, AClosedTargetsMemoryServesTheNextOfItsKind)
{
    gm20b::Texture first, opened;
    compressedSecond(first, opened, [&](gm20b::Texture &x) {
        return dev.createCompressedTexture(kW, kH, kBGRA8, gm20b::kCompKindC32, x) ||
               dev.createTexture(kW, kH, kBGRA8, x, false, true);
    });
    const uint64_t va = opened.va, handles = kernel_model::model.nextHandle;
    const size_t freed = kernel_model::model.freed.size();
    dev.destroyTexture(opened);
    EXPECT_EQ(kernel_model::model.freed.size(), freed);
    gm20b::Texture again;
    ASSERT_TRUE(dev.createCompressedTexture(kW, kH, kBGRA8, gm20b::kCompKindC32, again));
    EXPECT_EQ(again.va, va);
    EXPECT_EQ(kernel_model::model.nextHandle, handles) << "no BoAlloc";
    dev.destroyTexture(again);
    dev.destroyTexture(first);
}

// An ask refused for a shortage waits for memory given back (comp.h CompQueue::refuse): memory that cannot serve it
// goes back when its texture goes, as before, so that ask may be made again
TEST_F(DevicePrivateComp, AClosedTargetsMemoryGoesBackWhereARefusedAskWantsMore)
{
    gm20b::Texture first, opened, larger;
    compressedSecond(first, opened, [&](gm20b::Texture &x) {
        return dev.createCompressedTexture(kW, kH, kBGRA8, gm20b::kCompKindC32, x) ||
               dev.createTexture(kW, kH, kBGRA8, x, false, true);
    });
    const uint64_t refused = dev.stats.compFailed.load();
    kernel_model::model.refuseAllocCount = 1;
    ASSERT_FALSE(dev.createCompressedTexture(2 * kW, kH, kBGRA8, gm20b::kCompKindC32, larger));
    settle();
    ASSERT_GT(dev.stats.compFailed.load(), refused) << "the larger ask was refused";
    const size_t freed = kernel_model::model.freed.size();
    dev.destroyTexture(opened);
    EXPECT_GT(kernel_model::model.freed.size(), freed) << "given back to the kernel";
    EXPECT_EQ(gm20b::DeviceHarness::compReady(dev), 0u);
    dev.destroyTexture(first);
}

// A block-linear header moved to another address names it, as fx::mipChainTic's of that address does
TEST(PrivateCompHeaders, AMovedHeaderIsTheHeaderOfItsNewAddress)
{
    gm20b::fx::MipChain c;
    ASSERT_TRUE(gm20b::fx::mipChainLayout(256, 128, 4, 4, c));
    for (const auto &[from, to] : {std::pair<uint64_t, uint64_t>{0xc100520000, 0xc100580000},
                                   {0x1000000200, 0xffff0003fe00},
                                   {0xc100520000 + 0x40000, 0xc1005c0000}}) {
        uint32_t moved[8], made[8];
        gm20b::fx::mipChainTic(c, from, 0x12345678, true, 1, 3, moved, gm20b::fx::kTexTwoD);
        EXPECT_EQ(gm20b::ticAddress(moved), from);
        gm20b::ticAt(moved, to);
        gm20b::fx::mipChainTic(c, to, 0x12345678, true, 1, 3, made, gm20b::fx::kTexTwoD);
        EXPECT_EQ(memcmp(moved, made, 32), 0) << std::hex << from << " to " << to;
    }
}

// RAWRT-61: a view of a compressed private texture names its compressed memory through a header of its own over the
// same binding
TEST_F(DevicePrivateComp, AViewKeepsCompressedMemory)
{
    gm20b::Texture first, t, view;
    compressedSecond(first, t, [&](gm20b::Texture &x) {
        return dev.createCompressedTexture(128, 128, kBGRA8, gm20b::kCompKindC32, x) ||
               dev.createTexture(128, 128, kBGRA8, x, false, true);
    });
    ASSERT_TRUE(dev.createView(t, 81, view));  // BGRA8Unorm_sRGB
    EXPECT_TRUE(view.comptags) << "a raw view names the compressed memory";
    EXPECT_EQ(view.va, t.va);
    EXPECT_NE(view.tic, t.tic);
    dev.destroyTexture(view);
    dev.destroyTexture(t);
    dev.destroyTexture(first);
}

// gpu.x1.blend_exact's 4096x2056 BGRA8 target (boot 19 and boot 21: "compressed memory 40960 KiB kind 0xdb not made"):
// its compressed memory is its own comptag lines (2176 block-linear rows of 16 KiB: 272 lines, 34 MiB), not the 40 MiB
// size class a backing would take
TEST_F(DevicePrivateComp, APrivateTextureAsksItsOwnCompTagLines)
{
    gm20b::Texture t;
    ASSERT_FALSE(dev.createCompressedTexture(4096, 2056, kBGRA8, gm20b::kCompKindC32, t)) << "none made yet: asked";
    awaitMade();
    EXPECT_EQ(gm20b::DeviceHarness::compReadyBytes(dev), (std::vector<uint64_t>{272ull << 17}));
}

// comp.h storedZetaKind: a stored one-sample Depth32Float texture on ZF32_2CZ memory the compressed-memory thread
// made, kept made for the next of its kind when the texture goes, as a colour target's is
TEST_F(DevicePrivateComp, AStoredDepth32FloatTakesZF32_2CZAndKeepsItForTheNext)
{
    gm20b::Texture first, zeta;
    compressedSecond(
        first, zeta,
        [&](gm20b::Texture &t) {
            return dev.createOwnedZeta(64, 64, 252, 1, true, t);
        },
        false);
    EXPECT_TRUE(bound(gm20b::kCompKindZF32));
    EXPECT_NE(zeta.tic, 0u);
    const size_t freedBefore = kernel_model::model.freed.size(), ready = gm20b::DeviceHarness::compReady(dev);
    dev.destroyTexture(zeta);
    EXPECT_EQ(kernel_model::model.freed.size(), freedBefore);
    EXPECT_EQ(gm20b::DeviceHarness::compReady(dev), ready + 1) << "made for the next depth texture of its kind";
    gm20b::Texture next;
    ASSERT_TRUE(dev.createOwnedZeta(64, 64, 252, 1, true, next));
    EXPECT_TRUE(next.comptags);
    dev.destroyTexture(next);
    dev.destroyTexture(first);
}

// The compression thread's ZBC add held open in the kernel model (kernel_model::arriving, outside callLock) until
// released, then answered as a full table
class HeldZbc {
public:
    void arm()
    {
        std::lock_guard<std::mutex> l(lock_);
        held_ = open_ = false;
        kernel_model::arriving.store(&arrive, std::memory_order_release);
    }

    void awaitHeld()
    {
        std::unique_lock<std::mutex> l(lock_);
        wake_.wait(l, [] {
            return held_;
        });
    }

    void release()
    {
        std::lock_guard<std::mutex> l(lock_);
        open_ = true;
        wake_.notify_all();
    }

    ~HeldZbc()
    {
        release();
    }

private:
    static int arrive(uint32_t method, const void *)
    {
        if (method != TegraGM20BMethodZbc)
            return KERN_SUCCESS;
        std::unique_lock<std::mutex> l(lock_);
        held_ = true;
        wake_.notify_all();
        wake_.wait(l, [] {
            return open_;
        });
        return kIOReturnNoResources;
    }

    static inline std::mutex lock_;
    static inline std::condition_variable wake_;
    static inline bool held_ = false, open_ = false;
};

// The compression-settled event waits for the ZBC entries asked so far as for compressed memory: a block asked while
// the kernel holds the table's add runs once the add is answered, so HW Test's ZBC rounds need no gap between them
TEST_F(DevicePrivateComp, TheSettledEventWaitsForTheZbcEntriesAsked)
{
    HeldZbc zbc;
    zbc.arm();
    TegraGM20BAbiZbc e = {};
    e.type = TegraGM20BZbcColor;
    e.format = gm20b::kZbcA8R8G8B8;
    e.ds[0] = e.l2[0] = 0x3f000000;
    for (uint8_t i = 0; i < gm20b::ZbcTable::kZbcRepeats; i++)
        EXPECT_FALSE(gm20b::DeviceHarness::zbcHeld(dev, e));
    zbc.awaitHeld();
    std::atomic<bool> ran{false};
    std::promise<void> settled;
    dev.compWhenSettled([&] {
        ran.store(true);
        settled.set_value();
    });
    EXPECT_FALSE(ran.load()) << "ran while the ZBC entry was in the kernel's hands";
    zbc.release();
    settled.get_future().wait();
    EXPECT_FALSE(gm20b::DeviceHarness::zbcHeld(dev, e)) << "refused: the table is full";
}

class DeviceClearBox : public harness::DeviceTest {
protected:
    using Load = decltype(gm20b::ColorAttachment::load);
    gm20b::Texture c0, c1;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(kW, kH, kBGRA8, c0, false, true));
    }

    gm20b::Pipeline *state(const char *key, uint64_t hash)
    {
        gm20b::RuntimeRecord r;
        r.keyString = key;
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.pipeline.keyHash = hash;
        r.blob = {0x1a00, 0x1111};
        std::string why;
        gm20b::Pipeline *pl = dev.addRecord(r, false, why);
        EXPECT_NE(pl, nullptr) << why;
        return pl;
    }

    // One pass, c0 Clear and stored, c1 loaded as `load` and stored or not, one triangle under `scissor`: its clears
    std::vector<std::array<uint32_t, 5>> pass(Load load, bool store, bool c1Read, const uint32_t scissor[4])
    {
        gm20b::PassDesc p;
        p.color[0].texture = &c0;
        p.color[0].load = gm20b::ColorAttachment::Clear;
        p.color[0].store = true;
        p.color[1].texture = &c1;
        p.color[1].load = load;
        p.color[1].store = store;
        p.count = 2;
        gm20b::CommandBuffer *cb = dev.begin();
        gm20b::RenderEncoder *e = cb ? cb->render(p) : nullptr;
        if (!e) {
            ADD_FAILURE() << "no pass";
            return {};
        }
        e->setPipeline(c1Read ? state("samples=1 c0=80:15:0 c1=115:7:0 ds=0/0 ", 0xc1)
                              : state("samples=1 c0=80:15:0 c1=115:15:0 ds=0/0 ", 0xc0));
        e->setScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
        const char *why = nullptr;
        EXPECT_TRUE(e->draw(kTriangle, 0, 3, 1, 0, &why)) << (why ? why : "");
        e->end();
        const auto cleared = gm20b::DeviceHarness::clears(*e);
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        return cleared;
    }
};

using Clears = std::vector<std::array<uint32_t, 5>>;
const Clears kCardClears = {{0, 0, 0, kW, kH}, {1, 224, 128, 800, 384}};

TEST_F(DeviceClearBox, AMemorylessClearOnlyTheDrawsSeeCoversTheirTiles)
{
    ASSERT_TRUE(dev.createMemoryless(kW, kH, kRGBA16F, c1));
    const uint32_t card[4] = {250, 130, 520, 250};  // pixels 250,130 to 770,380: tiles 224,128 to 800,384
    const uint64_t narrowed = dev.stats.clearsNarrowed.load();
    EXPECT_EQ(pass(gm20b::ColorAttachment::Clear, false, true, card), kCardClears)
        << "the stored c0 keeps Metal's reach";
    EXPECT_EQ(dev.stats.clearsNarrowed.load(), narrowed + 1);
    EXPECT_EQ(pass(gm20b::ColorAttachment::Clear, false, false, card), (Clears{{0, 0, 0, kW, kH}}))
        << "no draw reads c1: nothing sees its clear";
    EXPECT_EQ(pass(gm20b::ColorAttachment::DontCare, false, true, card), kCardClears)
        << "a DontCare memoryless c1 a draw reads: zero over the draws' tiles";
    EXPECT_EQ(pass(gm20b::ColorAttachment::Clear, true, true, card), (Clears{{0, 0, 0, kW, kH}, {1, 0, 0, kW, kH}}))
        << "a memoryless c1 kept for a later pass (stored) clears whole";
    EXPECT_EQ(pass(gm20b::ColorAttachment::Clear, true, false, card), (Clears{{0, 0, 0, kW, kH}, {1, 0, 0, kW, kH}}));
    const uint32_t whole[4] = {0, 0, kW, kH};
    EXPECT_EQ(pass(gm20b::ColorAttachment::Clear, false, true, whole), (Clears{{0, 0, 0, kW, kH}, {1, 0, 0, kW, kH}}));
}

TEST_F(DeviceClearBox, AStoredClearKeepsMetalsReach)
{
    ASSERT_TRUE(dev.createTexture(kW, kH, kRGBA16F, c1, false, true));
    const uint32_t card[4] = {250, 130, 520, 250};
    EXPECT_EQ(pass(gm20b::ColorAttachment::Clear, true, true, card), (Clears{{0, 0, 0, kW, kH}, {1, 0, 0, kW, kH}}));
    EXPECT_EQ(pass(gm20b::ColorAttachment::Clear, false, true, card), kCardClears)
        << "a DontCare store: nothing after the pass reads it";
}

}  // namespace
