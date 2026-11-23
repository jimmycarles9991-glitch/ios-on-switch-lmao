// SPDX-License-Identifier: MIT
// A turned pass (turned_pass.h) on gm20b::Device whose attachment beside the turned target is loaded and stored and lies
// in a surface of rows 16 but not 32 bytes apart (gpu.x1.rotated_target's 90x60 frames): the texture header reads such a
// surface through its pool image (Texture::sampled), so the turn copy that moves the attachment into the pass's stand-in
// reads that image, which the copy engine must fill from the surface first. Read off the device's submitted words: copies
// (source, destination) and draws in order. And a Depth32Float and Stencil8 pair beside a turned target: each texture moved
// into a stand-in of the plane's, which the pass's Depth32Float_Stencil8 is laid out from.
#include "device_harness.h"
#include "../pack.h"
#include "../record.h"
#include "../turned_pass.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t kBGRA8 = 80, kDepth32Float = 252, kStencil8 = 253;
constexpr uint32_t kMacroCalls = 0x3800, kCeLaunch = 0x0300, kCeInUpper = 0x0400, kCeInLower = 0x0404,
                   kCeOutUpper = 0x0408, kCeOutLower = 0x040c;

struct Event {
    bool draw;
    uint64_t from, to;
};

std::vector<Event> submitted(size_t first)
{
    std::vector<Event> out;
    uint64_t in = 0, to = 0;
    auto method = [&](uint32_t subc, uint32_t m, uint32_t v) {
        if (subc == gm20b::SUBC_COPY) {
            if (m == kCeInUpper)
                in = (in & 0xffffffffull) | uint64_t(v) << 32;
            else if (m == kCeInLower)
                in = (in & ~0xffffffffull) | v;
            else if (m == kCeOutUpper)
                to = (to & 0xffffffffull) | uint64_t(v) << 32;
            else if (m == kCeOutLower)
                to = (to & ~0xffffffffull) | v;
            else if (m == kCeLaunch && in && to)
                out.push_back({false, in, to});
        } else if (subc == gm20b::SUBC_3D && m >= kMacroCalls) {
            out.push_back({true, 0, 0});
        }
    };
    const auto &jobs = kernel_model::model.jobs;
    for (size_t j = first; j < jobs.size(); j++) {
        uint32_t op = 0, subc = 0, m = 0, left = 0;
        for (const uint32_t w : jobs[j].words) {
            if (left) {
                method(subc, m, w);
                left--;
                if (op == 1 || op == 5)
                    m += 4;
                if (op == 5)
                    op = 3;
                continue;
            }
            if (!w)
                continue;
            op = w >> 29;
            subc = w >> 13 & 7;
            m = (w & 0xfff) << 2;
            if (op == 4)
                method(subc, m, w >> 16 & 0x1fff);
            else
                left = w >> 16 & 0x1fff;
        }
    }
    return out;
}

class NoDraws final : public gm20b::DrawReplay {
public:
    bool any() const override
    {
        return false;
    }

    bool atFrameResolution() const override
    {
        return false;
    }

    bool replay(gm20b::RenderEncoder &, const char **) override
    {
        return true;
    }
};

// IOSurface-like page-aligned memory a surface texture is made over
struct Surface {
    void *base = nullptr;
    uint64_t bytes = 0;

    Surface(uint32_t pitch, uint32_t height)
    {
        const size_t page = size_t(getpagesize());
        bytes = (uint64_t(pitch) * height + page - 1) / page * page;
        base = aligned_alloc(page, bytes);
        memset(base, 0x5a, bytes);
    }

    ~Surface()
    {
        free(base);
    }
};

class DeviceTurnedPass : public harness::DeviceTest {
protected:
    void SetUp() override
    {
        DeviceTest::SetUp();
        gm20b::RuntimeRecord r;
        r.keyString = "samples=1 c0=53 ";
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        const char *state = gm20b::turnCopyState(4);
        r.pipeline.keyHash = gm20b::Pack::fnv(state, strlen(state));
        r.blob = {0x1a00, 0x1111};
        std::string why;
        ASSERT_NE(dev.addRecord(r, false, why), nullptr) << why;
    }

    // A texture of `rotation` over `s`, its frame w x h (the plane h x w for a quarter turn), rows `pitch` bytes apart
    bool plane(Surface &s, uint8_t rotation, uint32_t w, uint32_t h, uint32_t pitch, gm20b::Texture &t)
    {
        const bool quarter = rotation == 1 || rotation == 3 || rotation == 4;
        const uint32_t pw = quarter ? h : w, ph = quarter ? w : h;
        if (!dev.createSurfaceTexture(0, s.base, s.bytes, nullptr, 0, pw, ph, pitch, kBGRA8, t, true))
            return false;
        t.rotation = rotation;
        return true;
    }

    // A pass into `target`, cleared and stored, beside `beside`, loaded and stored, with no draw: what it submitted, or why
    // not ("" encoded)
    std::string pass(gm20b::Texture &target, gm20b::Texture &beside, std::vector<Event> &out)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        const size_t first = kernel_model::model.jobs.size();
        gm20b::PassDesc p;
        p.color[0].texture = &target;
        p.color[0].load = gm20b::ColorAttachment::Clear;
        p.color[0].store = true;
        p.color[1].texture = &beside;
        p.color[1].load = gm20b::ColorAttachment::Load;
        p.color[1].store = true;
        p.count = 2;
        NoDraws draws;
        gm20b::TurnedPass turned;
        const char *why = "";
        gm20b::RenderEncoder *e = nullptr;
        if (!turned.begin(*cb, p, draws, &why) || !(e = cb->render(p, &why))) {
            cb->abandon();
            return why ? why : "no pass";
        }
        e->end();
        uint32_t seqno = 0;
        if (!turned.end(*cb, &why) || !cb->commit(seqno)) {
            cb->abandon();
            return why ? why : "not committed";
        }
        out = submitted(first);
        return "";
    }

    // The index of the first copy from `t` into its pool image, and of the first draw (events.size(): none)
    static void order(const std::vector<Event> &events, const gm20b::Texture &t, size_t &fill, size_t &draw)
    {
        fill = draw = events.size();
        for (size_t i = 0; i < events.size(); i++) {
            if (events[i].draw && draw == events.size())
                draw = i;
            if (!events[i].draw && t.sampled && events[i].from == t.va && events[i].to == t.sampled->va &&
                fill == events.size())
                fill = i;
        }
    }
};

TEST_F(DeviceTurnedPass, ALoadedPlaneOfSixteenByteRowsFillsItsPoolImageBeforeTheTurnCopyReadsIt)
{
    Surface targetMemory(240, 90), besideMemory(368, 60);
    gm20b::Texture target, beside;
    ASSERT_TRUE(plane(targetMemory, 1, 90, 60, 240, target));
    ASSERT_TRUE(plane(besideMemory, 2, 90, 60, 368, beside));
    ASSERT_NE(beside.sampled, nullptr) << "rows 368 bytes apart: the header reads a pool image";
    EXPECT_EQ(beside.tic, beside.sampled->tic);
    std::vector<Event> events;
    ASSERT_EQ(pass(target, beside, events), "");
    size_t fill = 0, draw = 0;
    order(events, beside, fill, draw);
    ASSERT_LT(draw, events.size()) << "the turn copies draw";
    ASSERT_LT(fill, events.size()) << "no copy of the loaded plane into the pool image its turn copy reads";
    EXPECT_LT(fill, draw) << "the pool image filled after the turn copy read it";
}

TEST_F(DeviceTurnedPass, ALoadedTextureNotTurnedOfSixteenByteRowsFillsItsPoolImageFirst)
{
    Surface targetMemory(240, 90), besideMemory(368, 60);
    gm20b::Texture target, beside;
    ASSERT_TRUE(plane(targetMemory, 1, 90, 60, 240, target));
    ASSERT_TRUE(plane(besideMemory, 0, 90, 60, 368, beside));
    ASSERT_NE(beside.sampled, nullptr);
    std::vector<Event> events;
    ASSERT_EQ(pass(target, beside, events), "");
    size_t fill = 0, draw = 0;
    order(events, beside, fill, draw);
    ASSERT_LT(fill, events.size()) << "no copy of the loaded texture into the pool image its turn copy reads";
    EXPECT_LT(fill, draw);
}

TEST_F(DeviceTurnedPass, APlaneOfAlignedRowsIsReadInPlace)
{
    Surface targetMemory(256, 96), besideMemory(512, 64);
    gm20b::Texture target, beside;
    ASSERT_TRUE(plane(targetMemory, 1, 96, 64, 256, target));
    ASSERT_TRUE(plane(besideMemory, 2, 96, 64, 512, beside));
    ASSERT_EQ(beside.sampled, nullptr);
    const std::array<uint32_t, 8> h = gm20b::DeviceHarness::header(dev, beside.tic);
    EXPECT_EQ(uint64_t(h[2] & 0xffff) << 32 | (h[1] & ~31u), beside.va)
        << "rows 512 bytes apart: the header reads the plane";
    std::vector<Event> events;
    ASSERT_EQ(pass(target, beside, events), "");
}

TEST_F(DeviceTurnedPass, ASeparateDepthAndStencilPairBesideATurnedTargetMovesIntoStandInsOfThePlane)
{
    gm20b::RuntimeRecord r;
    r.keyString = "samples=1 ds=252/0 ";
    for (gm20b::CapturedProgram &program : r.programs) {
        program.image.assign(80 + 64, 0);
        program.gprs = 8;
    }
    const char *state = gm20b::turnDepthState(kDepth32Float);
    r.pipeline.keyHash = gm20b::Pack::fnv(state, strlen(state));
    r.blob = {0x1a00, 0x1111};
    std::string why;
    ASSERT_NE(dev.addRecord(r, false, why), nullptr) << why;
    Surface targetMemory(256, 96);
    gm20b::Texture target, depth, stencil;
    ASSERT_TRUE(plane(targetMemory, 1, 96, 64, 256, target));
    ASSERT_TRUE(dev.createOwnedZeta(96, 64, kDepth32Float, 1, true, depth));
    ASSERT_TRUE(dev.createOwnedZeta(96, 64, kStencil8, 1, false, stencil));
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    gm20b::PassDesc p;
    p.color[0].texture = &target;
    p.color[0].load = gm20b::ColorAttachment::Clear;
    p.color[0].store = true;
    p.count = 1;
    p.depthStencil.texture = &depth;
    p.depthStencil.clearStencil = true;
    p.separate = {&depth, &stencil, true, false, true, false};
    NoDraws draws;
    gm20b::TurnedPass turned;
    const char *rule = "";
    ASSERT_TRUE(turned.begin(*cb, p, draws, &rule)) << rule;
    ASSERT_NE(p.separate.depth, &depth) << "the depth texture moved into a stand-in";
    ASSERT_NE(p.separate.stencil, &stencil) << "the stencil texture laid out as the plane too";
    EXPECT_EQ(p.depthStencil.texture, p.separate.depth);
    for (const gm20b::Texture *t : {p.separate.depth, p.separate.stencil}) {
        EXPECT_EQ(t->width, 64u) << "the plane's width, the frame's height";
        EXPECT_EQ(t->height, 96u);
        EXPECT_EQ(t->rotation, 1u);
    }
    EXPECT_EQ(p.separate.depth->format, kDepth32Float);
    EXPECT_EQ(p.separate.stencil->format, kStencil8);
    const uint64_t depthStandIn = p.separate.depth->va;
    gm20b::RenderEncoder *e = cb->render(p, &rule);
    ASSERT_NE(e, nullptr) << rule;
    e->end();
    ASSERT_TRUE(turned.end(*cb, &rule)) << rule;
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const std::vector<Event> events = submitted(first);
    const auto copied = [&](uint64_t from, uint64_t to) {
        return std::any_of(events.begin(), events.end(), [&](const Event &x) {
            return !x.draw && x.from == from && x.to == to;
        });
    };
    EXPECT_FALSE(copied(depth.va, depthStandIn)) << "a turn copy draws the stand-in, no copy-engine rows";
    EXPECT_GE(std::count_if(events.begin(), events.end(),
                            [](const Event &x) {
                                return x.draw;
                            }),
              2)
        << "the loaded depth's turn copy in and its stored turn copy back";
}

// A level's or slice's part beside a turned target is the encoder's copy for one pass (ColorAttachment::image): its turn
// copy reads the texture's own header of that part, and a part without one is never given one, which nothing would free
TEST_F(DeviceTurnedPass, ALoadedPartBesideATurnedTargetMakesNoHeaderOfItsOwn)
{
    Surface targetMemory(256, 96), besideMemory(512, 64);
    gm20b::Texture target, beside;
    ASSERT_TRUE(plane(targetMemory, 1, 96, 64, 256, target));
    ASSERT_TRUE(plane(besideMemory, 0, 96, 64, 512, beside));
    const auto began = [&] {
        gm20b::Texture part = beside;
        part.tic = 0;
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb)
            return true;
        gm20b::PassDesc p;
        p.color[0].texture = &target;
        p.color[0].load = gm20b::ColorAttachment::Clear;
        p.color[0].store = true;
        p.color[1].texture = &part;
        p.color[1].image = &beside;
        p.color[1].load = gm20b::ColorAttachment::Load;
        p.color[1].store = true;
        p.count = 2;
        NoDraws draws;
        gm20b::TurnedPass turned;
        const char *why = "";
        const bool begun = turned.begin(*cb, p, draws, &why);
        cb->abandon();
        return begun;
    };
    (void)began();  // the pass's stand-in backings, made once and kept
    const uint32_t headers = dev.headersLeft();
    EXPECT_FALSE(began()) << "a part with no header of its own";
    EXPECT_EQ(dev.headersLeft(), headers) << "a header made for the pass's copy of the part";
}

}  // namespace
