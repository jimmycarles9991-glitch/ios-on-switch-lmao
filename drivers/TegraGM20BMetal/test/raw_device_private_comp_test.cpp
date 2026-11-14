// SPDX-License-Identifier: MIT
// raw/runtime/raw_private_comp.cpp on raw::Device over the kernel model: private chains, arrays and stored multisampled
// textures on compressed memory made off the render thread (TextureAsk::compressKind), and the move to plain memory
// before an m2v twin, every level, layer and sample copied by the copy engine.
#include "raw_device_harness.h"
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

constexpr uint32_t kBGRA8 = 80;

// The copy engine's source and destination of each launch the jobs since `first` submitted
std::vector<std::pair<uint64_t, uint64_t>> copies(size_t first)
{
    std::vector<std::pair<uint64_t, uint64_t>> out;
    const auto &jobs = kernel_model::model.jobs;
    for (size_t j = first; j < jobs.size(); j++)
        for (const TegraGM20BAbiPush &push : jobs[j].pushes) {
            const auto *w = reinterpret_cast<const uint32_t *>(kernel_model::model.host(push.va));
            uint32_t in[2] = {}, outs[2] = {};
            for (uint32_t i = 0; w && i < push.dwords;) {
                const uint32_t hdr = w[i++], op = hdr >> 29, subc = hdr >> 13 & 7, m = (hdr & 0xfff) << 2,
                               n = hdr >> 16 & 0x1fff;
                if (!hdr)
                    continue;
                if (op == 7)
                    break;
                if (op == 4) {
                    if (subc == 4 && m == NVB0B5_LAUNCH_DMA)
                        out.push_back({uint64_t(in[0]) << 32 | in[1], uint64_t(outs[0]) << 32 | outs[1]});
                    continue;
                }
                for (uint32_t k = 0; k < n && i < push.dwords; k++, i++) {
                    const uint32_t method = m + 4 * (op == 1 ? k : op == 5 && k ? 1 : 0);
                    if (subc != 4)
                        continue;
                    if (method == NVB0B5_OFFSET_IN_UPPER)
                        in[0] = w[i];
                    else if (method == NVB0B5_OFFSET_IN_LOWER)
                        in[1] = w[i];
                    else if (method == NVB0B5_OFFSET_OUT_UPPER)
                        outs[0] = w[i];
                    else if (method == NVB0B5_OFFSET_OUT_LOWER)
                        outs[1] = w[i];
                    else if (method == NVB0B5_LAUNCH_DMA)
                        out.push_back({uint64_t(in[0]) << 32 | in[1], uint64_t(outs[0]) << 32 | outs[1]});
                }
            }
        }
    return out;
}

class DevicePrivateComp : public harness::DeviceTest {
protected:
    std::mutex kindsLock;
    std::vector<uint32_t> kinds;  // each VmBind's kind, the compressed-memory thread's among them

    void SetUp() override
    {
        kernel_model::model = {};
        kernel_model::model.infoFlags = TegraGM20BInfoCompression;
        kernel_model::model.bound = [this](const TegraGM20BAbiBindOp &op) {
            std::lock_guard<std::mutex> l(kindsLock);
            kinds.push_back(op.kind);
        };
        std::string err;
        ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, raw::Device::Role::Client)) << err;
    }

    bool bound(uint32_t kind)
    {
        std::lock_guard<std::mutex> l(kindsLock);
        return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
    }

    // The test's wait for the compressed-memory thread to answer the device's first ask
    void awaitMade()
    {
        for (int i = 0; i < 5000 && !raw::DeviceHarness::compReady(dev); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ASSERT_TRUE(raw::DeviceHarness::compReady(dev)) << "no compressed memory was made";
    }

    // `make` once (plain: none of its size made yet, and asked for), then again on what was made
    // (`colour`: one the colour move takes, plainFromCompressed; a zeta moves into its m2v twin's image instead)
    template <typename Make>
    void compressedSecond(raw::Texture &first, raw::Texture &second, Make make, bool colour = true)
    {
        ASSERT_TRUE(make(first));
        EXPECT_FALSE(first.comptags);
        awaitMade();
        ASSERT_TRUE(make(second));
        EXPECT_TRUE(second.comptags);
        EXPECT_EQ(dev.privateCompressed(second), colour);
    }
};

TEST_F(DevicePrivateComp, AChainTakesCompressedMemoryAndMovesEveryLevelToPlain)
{
    raw::Texture first, chain;
    compressedSecond(first, chain, [&](raw::Texture &t) {
        return dev.createMipTexture(256, 256, kBGRA8, 5, t, nullptr, raw::kCompKindC32);
    });
    EXPECT_TRUE(bound(raw::kCompKindC32));
    ASSERT_TRUE(chain.chain);
    EXPECT_EQ(chain.chain->va, chain.va);
    EXPECT_EQ(chain.cpu, nullptr) << "no CPU view of compressed memory";
    raw::Texture level;
    ASSERT_TRUE(raw::chainLevelImage(chain, 1, level));
    EXPECT_TRUE(level.comptags) << "level 1 is rendered with SET_COLOR_COMPRESSION, as level 0 is";
    ASSERT_TRUE(raw::chainLevelTarget(chain, 2, level));
    EXPECT_TRUE(level.comptags);
    const uint64_t compressed = chain.va;
    const uint32_t tic = chain.tic;
    const raw::fx::MipChain layout = chain.chain->layout;
    const size_t jobs = kernel_model::model.jobs.size();
    ASSERT_TRUE(dev.plainFromCompressed(chain));
    EXPECT_FALSE(chain.comptags);
    EXPECT_FALSE(dev.privateCompressed(chain));
    EXPECT_EQ(chain.tic, tic) << "rewritten in place by the move's job";
    ASSERT_TRUE(chain.chain);
    EXPECT_EQ(chain.chain->va, chain.va);
    EXPECT_NE(chain.va, compressed);
    ASSERT_TRUE(raw::chainLevelImage(chain, 1, level));
    EXPECT_FALSE(level.comptags);
    const auto launched = copies(jobs);
    for (uint32_t level = 0; level < layout.levels; level++)
        EXPECT_NE(
            std::find(launched.begin(), launched.end(),
                      std::make_pair(compressed + layout.level[level].offset, chain.va + layout.level[level].offset)),
            launched.end())
            << "level " << level;
    dev.destroyTexture(chain);
    dev.destroyTexture(first);
}

TEST_F(DevicePrivateComp, AnArrayMovesEveryLayer)
{
    raw::Texture first, array;
    compressedSecond(first, array, [&](raw::Texture &t) {
        return dev.createArray(128, 128, 3, kBGRA8, raw::fx::kTexTwoDArray, t, 1, nullptr, raw::kCompKindC32);
    });
    const uint64_t compressed = array.va, stride = array.layerBytes;
    ASSERT_EQ(array.layers, 3u);
    const size_t jobs = kernel_model::model.jobs.size();
    ASSERT_TRUE(dev.plainFromCompressed(array));
    EXPECT_FALSE(array.comptags);
    const auto launched = copies(jobs);
    for (uint32_t layer = 0; layer < 3; layer++)
        EXPECT_NE(std::find(launched.begin(), launched.end(),
                            std::make_pair(compressed + layer * stride, array.va + layer * stride)),
                  launched.end())
            << "layer " << layer;
    dev.destroyTexture(array);
    dev.destroyTexture(first);
}

TEST_F(DevicePrivateComp, AStoredMultisampledTextureMovesAsItsSampleGrid)
{
    raw::Texture first, ms;
    const uint32_t kind = raw::compressedKind(4, 0, 4);
    compressedSecond(first, ms, [&](raw::Texture &t) {
        return dev.createStoredMultisample(64, 64, kBGRA8, 4, t, nullptr, kind);
    });
    EXPECT_TRUE(bound(kind)) << "C32_MS4_2CRA";
    const uint64_t compressed = ms.va;
    const size_t jobs = kernel_model::model.jobs.size();
    ASSERT_TRUE(dev.plainFromCompressed(ms));
    EXPECT_FALSE(ms.comptags);
    EXPECT_EQ(ms.samples, 4u);
    const auto launched = copies(jobs);
    ASSERT_FALSE(launched.empty());
    EXPECT_EQ(launched.front(), std::make_pair(compressed, ms.va));
    dev.destroyTexture(ms);
    dev.destroyTexture(first);
}

TEST_F(DevicePrivateComp, AMovedTexturesCompressedMemoryGoesBackWithIt)
{
    raw::Texture first, t;
    compressedSecond(first, t, [&](raw::Texture &x) {
        return dev.createMipTexture(128, 128, kBGRA8, 3, x, nullptr, raw::kCompKindC32);
    });
    ASSERT_TRUE(dev.plainFromCompressed(t));
    const size_t freedBefore = kernel_model::model.freed.size();
    dev.destroyTexture(t);
    EXPECT_GT(kernel_model::model.freed.size(), freedBefore) << "the compressed buffer went back to the kernel";
    dev.destroyTexture(first);
}

// A block-linear header moved to another address names it, as fx::mipChainTic's of that address does
TEST(PrivateCompHeaders, AMovedHeaderIsTheHeaderOfItsNewAddress)
{
    raw::fx::MipChain c;
    ASSERT_TRUE(raw::fx::mipChainLayout(256, 128, 4, 4, c));
    for (const auto &[from, to] : {std::pair<uint64_t, uint64_t>{0xc100520000, 0xc100580000},
                                   {0x1000000200, 0xffff0003fe00},
                                   {0xc100520000 + 0x40000, 0xc1005c0000}}) {
        uint32_t moved[8], made[8];
        raw::fx::mipChainTic(c, from, 0x12345678, true, 1, 3, moved, raw::fx::kTexTwoD);
        EXPECT_EQ(raw::ticAddress(moved), from);
        raw::ticAt(moved, to);
        raw::fx::mipChainTic(c, to, 0x12345678, true, 1, 3, made, raw::fx::kTexTwoD);
        EXPECT_EQ(memcmp(moved, made, 32), 0) << std::hex << from << " to " << to;
    }
}

// RAWRT-61: a view of a compressed private texture names its compressed memory; the move before an m2v twin rewrites
// the texture's header and the view's in place on the GPU (the same indices, so a gpuResourceID handed out stays good)
// and moves the view's address with it
TEST_F(DevicePrivateComp, AViewKeepsCompressedMemoryAndFollowsTheMove)
{
    raw::Texture first, t, view;
    compressedSecond(first, t, [&](raw::Texture &x) {
        return dev.createCompressedTexture(128, 128, kBGRA8, raw::kCompKindC32, x) ||
               dev.createTexture(128, 128, kBGRA8, x, false, true);
    });
    ASSERT_TRUE(dev.createView(t, 81, view));  // BGRA8Unorm_sRGB
    EXPECT_TRUE(view.comptags) << "a raw view names the compressed memory";
    EXPECT_EQ(view.va, t.va);
    const uint32_t tic = t.tic, viewTic = view.tic;
    const std::array<uint32_t, 8> before = raw::DeviceHarness::header(dev, tic);
    const size_t jobs = kernel_model::model.jobs.size();
    ASSERT_TRUE(dev.plainFromCompressed(t));
    EXPECT_EQ(t.tic, tic) << "the texture's header index is kept";
    EXPECT_FALSE(view.comptags);
    EXPECT_EQ(view.va, t.va) << "the view names the plain memory";
    EXPECT_EQ(view.tic, viewTic);
    const auto launched = copies(jobs);
    const auto into = [&](uint64_t va) {
        return std::find_if(launched.begin(), launched.end(), [&](const auto &l) {
                   return l.second == va;
               }) != launched.end();
    };
    EXPECT_TRUE(into(raw::DeviceHarness::ticVa(dev, tic))) << "the texture's header rewritten by the move's job";
    for (const auto &[src, dst] : launched)
        if (dst == raw::DeviceHarness::ticVa(dev, tic)) {
            uint32_t written[8];
            memcpy(written, kernel_model::model.host(src), 32);
            uint32_t want[8];
            memcpy(want, before.data(), 32);
            raw::ticAt(want, t.va);
            EXPECT_EQ(memcmp(written, want, 32), 0) << "its own header (swizzle, format) naming the plain memory";
        }
    EXPECT_TRUE(into(raw::DeviceHarness::ticVa(dev, viewTic))) << "the view's header rewritten by the move's job";
    dev.destroyTexture(view);
    dev.destroyTexture(t);
    dev.destroyTexture(first);
}

// gpu.x1.blend_exact's 4096x2056 BGRA8 target (boot 19 and boot 21: "compressed memory 40960 KiB kind 0xdb not made"):
// its compressed memory is its own comptag lines (2176 block-linear rows of 16 KiB: 272 lines, 34 MiB), not the 40 MiB
// size class a backing would take
TEST_F(DevicePrivateComp, APrivateTextureAsksItsOwnCompTagLines)
{
    raw::Texture t;
    ASSERT_FALSE(dev.createCompressedTexture(4096, 2056, kBGRA8, raw::kCompKindC32, t)) << "none made yet: asked";
    awaitMade();
    EXPECT_EQ(raw::DeviceHarness::compReadyBytes(dev), (std::vector<uint64_t>{272ull << 17}));
}

// raw_comp.h storedZetaKind: a stored one-sample Depth32Float texture on ZF32_2CZ memory the compressed-memory thread
// made, given back with the texture
TEST_F(DevicePrivateComp, AStoredDepth32FloatTakesZF32_2CZAndGivesItBack)
{
    raw::Texture first, zeta;
    compressedSecond(
        first, zeta,
        [&](raw::Texture &t) {
            return dev.createOwnedZeta(64, 64, 252, 1, true, t);
        },
        false);
    EXPECT_TRUE(bound(raw::kCompKindZF32));
    EXPECT_NE(zeta.tic, 0u);
    const size_t freedBefore = kernel_model::model.freed.size();
    dev.destroyTexture(zeta);
    EXPECT_GT(kernel_model::model.freed.size(), freedBefore);
    dev.destroyTexture(first);
}

}  // namespace
