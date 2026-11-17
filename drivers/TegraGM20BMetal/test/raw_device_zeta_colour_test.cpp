// SPDX-License-Identifier: MIT
// raw/runtime/raw_zeta_colour.h's stencil planes on raw::Device, read back as the methods it submitted (the kernel model keeps
// each job's push words): a turn copy and a stencil resolve take the route with no probe, bind no colour target over the
// zeta's pages, the copy engine fills a pool image from the zeta before the draw and copies it back after; hw_test_app's
// comparison arm takes the eight draws, its direct arm draws over the zeta's pages with the tiled cache off for its draw.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_mme.h"
#include "../raw/runtime/raw_record.h"
#include "../raw/runtime/raw_stencil_bits.h"
#include "../raw/runtime/raw_zeta_colour.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include "../../../kits/SwitchGM20B/SwitchGM20BCopy.h"
#include <cstring>

namespace {

using raw::StencilSample;

constexpr uint32_t kD32S8 = 260, kS8 = 253, kSide = 64;

struct Method {
    uint32_t subchannel, method, value;
};

// `words`' methods in order (INC, NON_INC, ONE_INC and IMMD headers)
std::vector<Method> methods(const std::vector<uint32_t> &words)
{
    std::vector<Method> out;
    for (size_t i = 0; i < words.size();) {
        const uint32_t h = words[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                       method = (h & 0xfff) << 2;
        if (op == 4) {
            out.push_back({subc, method, count});
            continue;
        }
        if (op != 1 && op != 3 && op != 5)
            continue;
        for (uint32_t k = 0; k < count && i < words.size(); k++, i++)
            out.push_back({subc, method + 4 * (op == 1 ? k : op == 5 && k ? 1 : 0), words[i]});
    }
    return out;
}

// A call of one of raw::mme's draw macros, the form raw::RenderEncoder::draw takes
bool drawMacro(uint32_t method)
{
    const uint32_t first = NVB197_CALL_MME_MACRO(0), last = NVB197_CALL_MME_MACRO(127);
    return method >= first && method <= last && (method - first) % 8 == 0 &&
           raw::mme::countedAsDraw((method - first) / 8);
}

// What the methods bind and move, in order: each colour target 0 address, copy engine offsets and draw
struct Event {
    enum Kind {
        Target,
        Copy,
        Draw,
        TiledCache
    } kind;

    uint64_t a = 0, b = 0;  // Target: its address; Copy: from, to; TiledCache: enabled
};

std::vector<Event> events(const std::vector<Method> &m)
{
    std::vector<Event> out;
    uint32_t hi[2] = {}, ctHi = 0;
    uint64_t in = 0, outVa = 0;
    for (const Method &x : m) {
        if (x.subchannel == raw::SUBC_COPY) {
            if (x.method == NVB0B5_OFFSET_IN_UPPER)
                hi[0] = x.value;
            else if (x.method == NVB0B5_OFFSET_IN_LOWER)
                in = uint64_t(hi[0]) << 32 | x.value;
            else if (x.method == NVB0B5_OFFSET_OUT_UPPER)
                hi[1] = x.value;
            else if (x.method == NVB0B5_OFFSET_OUT_LOWER)
                outVa = uint64_t(hi[1]) << 32 | x.value;
            else if (x.method == NVB0B5_LAUNCH_DMA)
                out.push_back({Event::Copy, in, outVa});
            continue;
        }
        if (x.subchannel != raw::SUBC_3D)
            continue;
        if (x.method == NVB197_SET_COLOR_TARGET_A(0))
            ctHi = x.value;
        else if (x.method == NVB197_SET_COLOR_TARGET_B(0))
            out.push_back({Event::Target, uint64_t(ctHi) << 32 | x.value});
        else if (x.method == NVB197_BEGIN || drawMacro(x.method))
            out.push_back({Event::Draw});
        else if (x.method == SWITCH_NVB197_SET_TILED_CACHE_ENABLE)
            out.push_back({Event::TiledCache, x.value});
    }
    return out;
}

class DeviceZetaColour : public harness::DeviceTest {
protected:
    void SetUp() override
    {
        kernel_model::model = {};
        kernel_model::model.infoFlags = TegraGM20BInfoTiledCache;
        std::string err;
        ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, raw::Device::Role::Client)) << err;
        for (uint32_t format : {kD32S8, kS8})
            for (uint32_t samples : {1u, 4u})
                ASSERT_NE(state(format, samples), nullptr) << format << " " << samples;
    }

    // The pack's state of the route into `format` from `samples` samples (sample 0), its write mask the pack line's
    raw::Pipeline *state(uint32_t format, uint32_t samples)
    {
        const char *name = raw::stencilColourState(format, samples, StencilSample::Zero);
        raw::RuntimeRecord r;
        r.keyString = std::string("samples=1 c0=") + std::to_string(raw::zetaColourFormat(format)) + " ds=0/0 ";
        for (raw::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.programs[1].id = name;
        r.pipeline.keyHash = raw::Pack::fnv(name, strlen(name));
        r.blob = {0x1a00, format == kD32S8 && samples > 1 ? 0x10u : 0x1111u};
        std::string why;
        return dev.addRecord(r, false, why);
    }

    raw::Texture zeta(uint32_t format, uint32_t samples)
    {
        raw::Texture t;
        EXPECT_TRUE(samples > 1 ? dev.createOwnedZeta(kSide, kSide, format, samples, t)
                                : dev.createOwnedZeta(kSide, kSide, format, 1, true, t))
            << format << " " << samples;
        return t;
    }

    // The events of one command buffer whose stencil plane into `into` is `write`'s; empty where it was refused
    template <class Write> std::vector<Event> plane(Write write, std::string *why)
    {
        raw::CommandBuffer *cb = dev.begin();
        if (!cb)
            return {};
        const char *refused = nullptr;
        uint32_t seqno = 0;
        if (!write(*cb, &refused) || !cb->commit(seqno)) {
            cb->abandon();
            *why = refused ? refused : "not committed";
            return {};
        }
        return events(methods(kernel_model::model.jobs.back().words));
    }

    template <class Source> std::vector<Event> turnCopy(raw::Texture &into, Source &from, std::string *why)
    {
        raw::Rotation r;
        r.turn = 1;
        r.width = r.height = kSide;
        const raw::TurnMap map = raw::within(raw::frameToPlane(r), {0, 0, kSide, kSide});
        return plane(
            [&](raw::CommandBuffer &cb, const char **refused) {
                return cb.zetaColourTakes(into, from, StencilSample::Zero) && cb.turnCopy(into, from, map, refused);
            },
            why);
    }

    std::vector<Event> resolve(raw::Texture &into, raw::Texture &from, std::string *why)
    {
        return plane(
            [&](raw::CommandBuffer &cb, const char **refused) {
                return cb.stencilResolve(into, from, StencilSample::Zero, kSide, kSide, refused);
            },
            why);
    }
};

// The draw binds a colour target off the zeta's pages, filled from the zeta by the copy engine before it and copied back
// into the zeta after it
void expectPoolImage(const std::vector<Event> &ev, uint64_t zeta, const char *what)
{
    size_t draw = ev.size();
    for (size_t i = 0; i < ev.size(); i++) {
        if (ev[i].kind == Event::Target)
            EXPECT_NE(ev[i].a, zeta) << what << ": a colour target over the zeta's pages";
        if (ev[i].kind == Event::Draw && draw == ev.size())
            draw = i;
    }
    ASSERT_LT(draw, ev.size()) << what << ": no draw";
    uint64_t target = 0;
    for (size_t i = 0; i < draw; i++)
        if (ev[i].kind == Event::Target)
            target = ev[i].a;
    bool filled = false, stored = false;
    for (size_t i = 0; i < ev.size(); i++)
        if (ev[i].kind == Event::Copy) {
            filled |= i < draw && ev[i].a == zeta && ev[i].b == target;
            stored |= i > draw && ev[i].a == target && ev[i].b == zeta;
        }
    EXPECT_TRUE(filled) << what << ": the pool image is not filled from the zeta before the draw";
    EXPECT_TRUE(stored) << what << ": the pool image is not copied back into the zeta after the draw";
}

TEST_F(DeviceZetaColour, ATurnCopyRendersThroughAPoolImageTheCopyEngineMoves)
{
    for (uint32_t format : {kD32S8, kS8}) {
        raw::Texture from = zeta(format, 1), into = zeta(format, 1);
        std::string why;
        const std::vector<Event> ev = turnCopy(into, from, &why);
        ASSERT_FALSE(ev.empty()) << format << ": " << why;
        expectPoolImage(ev, into.va, format == kS8 ? "s8 turn copy" : "d32s8 turn copy");
    }
}

TEST_F(DeviceZetaColour, APreparedSourceKeepsTheOwnersTurnCopyCommandsAndHeader)
{
    const auto normalized = [](const std::vector<Event> &ev) {
        uint64_t scratch = 0;
        for (const Event &e : ev)
            if (e.kind == Event::Target && e.a)
                scratch = e.a;
        std::vector<std::array<uint64_t, 3>> out;
        for (const Event &e : ev)
            out.push_back({uint64_t(e.kind), e.a == scratch ? UINT64_MAX : e.a, e.b == scratch ? UINT64_MAX : e.b});
        return out;
    };
    for (uint32_t format : {kD32S8, kS8}) {
        SCOPED_TRACE(format);
        raw::Texture from, into = zeta(format, 1);
        ASSERT_TRUE(dev.createOwnedZeta(kSide, kSide, format, 1, false, from));
        ASSERT_EQ(from.tic, 0u);
        std::string why;
        const std::vector<Event> owned = turnCopy(into, from, &why);
        ASSERT_FALSE(owned.empty()) << why;
        const uint32_t tic = from.tic;
        ASSERT_NE(tic, 0u);
        const auto header = raw::DeviceHarness::header(dev, tic);
        const raw::Texture prepared = from;
        const std::vector<Event> ready = turnCopy(into, prepared, &why);
        ASSERT_FALSE(ready.empty()) << why;
        expectPoolImage(owned, into.va, "owned source");
        expectPoolImage(ready, into.va, "prepared source");
        EXPECT_EQ(normalized(owned), normalized(ready));
        EXPECT_EQ(from.tic, tic);
        EXPECT_EQ(prepared.tic, tic);
        EXPECT_EQ(raw::DeviceHarness::header(dev, tic), header);
        dev.destroyTexture(from);
        EXPECT_EQ(raw::DeviceHarness::header(dev, tic), raw::DeviceHarness::header(dev, 0));
        dev.destroyTexture(into);
    }
}

TEST_F(DeviceZetaColour, AStencilResolveRendersThroughAPoolImageTheCopyEngineMoves)
{
    for (uint32_t format : {kD32S8, kS8}) {
        raw::Texture from = zeta(format, 4), into = zeta(format, 1);
        std::string why;
        const std::vector<Event> ev = resolve(into, from, &why);
        ASSERT_FALSE(ev.empty()) << format << ": " << why;
        expectPoolImage(ev, into.va, format == kS8 ? "s8 resolve" : "d32s8 resolve");
    }
}

// hw_test_app's comparison arm takes the eight draws, the route's state at hand
TEST_F(DeviceZetaColour, TheComparisonArmTakesTheEightDraws)
{
    raw::Texture from = zeta(kD32S8, 1), into = zeta(kD32S8, 1);
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    EXPECT_TRUE(cb->zetaColourTakes(into, from, StencilSample::Zero));
    dev.probeStencilBits = true;
    EXPECT_FALSE(cb->zetaColourTakes(into, from, StencilSample::Zero));
    cb->abandon();
}

// hw_test_app's direct arm: the zeta's own pages the colour target, the tiled cache off around the draw alone
TEST_F(DeviceZetaColour, TheDirectArmDrawsOverTheZetaUntiled)
{
    dev.probeZetaDirect = true;
    raw::Texture from = zeta(kD32S8, 1), into = zeta(kD32S8, 1);
    std::string why;
    const std::vector<Event> ev = turnCopy(into, from, &why);
    ASSERT_FALSE(ev.empty()) << why;
    size_t draws = 0;
    int64_t tiled = -1;
    uint64_t target = 0;
    for (const Event &e : ev) {
        if (e.kind == Event::Target)
            target = e.a;
        if (e.kind == Event::Copy)
            EXPECT_TRUE(e.a != into.va && e.b != into.va) << "a copy of the zeta in the direct arm";
        if (e.kind == Event::TiledCache)
            tiled = int64_t(e.a);
        if (e.kind == Event::Draw) {
            draws++;
            EXPECT_EQ(target, into.va);
            EXPECT_EQ(tiled, 0) << "the tiled cache on at the draw";
        }
    }
    EXPECT_EQ(draws, 1u);
    EXPECT_EQ(tiled, 1) << "the tiled cache left off after the draw";
}

}  // namespace
