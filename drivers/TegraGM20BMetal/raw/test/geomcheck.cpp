// SPDX-License-Identifier: MIT
// raw GM20B path over the mock GPU: each geometry decision of the encode taken both ways, read from the pushes the mock
// decoded (PUSHDUMP). A draw fetching a colour through the texture unit (a state that fetches a colour it does not write
// draws its barrier form) whose pixels meet no earlier write takes no fetch barrier (the harness mark stands in its
// place) and one whose pixels meet them takes it; a stored target's Clear is left out under a first
// draw that covers it opaquely and kept under one that does not; a split state's draws take the split twin, its prologue
// launched on the compute engine before the pass, where the pass's split plan admits their pixels, and the whole program
// where it does not.
// usage: geomcheck <pack.bin> <corpus.log>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>
#include "../runtime/raw_gpu.h"
#include "frame.h"
#include "../../../../third_party/mesa/src/nouveau/headers/nvidia/classes/clb197.h"
#include "../../../../third_party/mesa/src/nouveau/headers/nvidia/classes/clb1c0.h"

namespace {

int failures = 0;

void expect(bool ok, const char *what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

constexpr uint32_t kMarkElided = 0x0e11de;  // raw_gpu.cpp's mark of a fetch barrier left out
constexpr uint32_t kSize = 256, kBGRA8 = 80;

// The methods the mock decoded since the last look: subchannel, method, value
struct Pushes {
    struct Method {
        uint32_t subchannel, method, value;
    };

    FILE *file = nullptr;
    long at = 0;

    std::vector<Method> since()
    {
        fflush(nullptr);
        std::vector<Method> out;
        fseek(file, at, SEEK_SET);
        char line[128];
        while (fgets(line, sizeof line, file)) {
            unsigned long long submit;
            unsigned subchannel, method, value;
            if (sscanf(line, "%llu %u %x %x", &submit, &subchannel, &method, &value) == 4)
                out.push_back({subchannel, method, value});
        }
        at = ftell(file);
        return out;
    }

    static unsigned count(const std::vector<Method> &all, uint32_t subchannel, uint32_t method,
                          bool (*holds)(uint32_t) = nullptr)
    {
        unsigned n = 0;
        for (const Method &m : all)
            n += m.subchannel == subchannel && m.method == method && (!holds || holds(m.value));
        return n;
    }
};

struct Frame {
    raw::Device &dev;
    raw::Texture target, scratch;
    uint8_t zeros[256] = {};

    raw::PassDesc pass(bool clear) const
    {
        raw::PassDesc pd;
        pd.count = 2;
        pd.color[0].texture = const_cast<raw::Texture *>(&target);
        pd.color[0].load = clear ? raw::ColorAttachment::Clear : raw::ColorAttachment::Load;
        pd.color[1].texture = const_cast<raw::Texture *>(&scratch);
        pd.color[1].load = raw::ColorAttachment::Clear;
        pd.color[1].store = false;
        return pd;
    }

    // A Core Animation draw of `p` over the pixels [x, x + w) x [y, y + h): its vertices and identity transform by
    // setBytes, zeros for the other buffers its functions read, a scissor of the same rectangle
    bool draw(raw::RenderEncoder *e, raw::Pipeline *p, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
    {
        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const auto clip = [](uint32_t pixel, bool vertical) {
            const float c = float(pixel) / kSize * 2 - 1;
            return vertical ? -c : c;
        };

        struct Vertex {
            float position[4], texcoord[4];
            uint16_t colour[4], pad[4];
        };

        const float x0 = clip(x, false), x1 = clip(x + w, false), y0 = clip(y, true), y1 = clip(y + h, true);
        const Vertex v[4] = {{{x0, y0, 0, 1}, {}, {}, {}},
                             {{x1, y0, 0, 1}, {}, {}, {}},
                             {{x0, y1, 0, 1}, {}, {}, {}},
                             {{x1, y1, 0, 1}, {}, {}, {}}};
        e->setPipeline(p);
        e->setBytes(1, 1, v, sizeof v);
        e->setBytes(1, 2, identity, sizeof identity);
        for (uint32_t slot : {0u, 3u})
            e->setBytes(1, slot, zeros, sizeof zeros);
        for (uint32_t slot = 0; slot < 4; slot++)
            e->setBytes(2, slot, zeros, sizeof zeros);
        e->setScissor(x, y, w, h);
        const char *why = nullptr;
        if (e->draw(4, 0, 4, 1, 0, &why))
            return true;
        std::printf("      draw refused: %s\n", why ? why : "-");
        return false;
    }
};

bool commit(raw::Device &dev, raw::CommandBuffer *cb)
{
    uint32_t seq = 0;
    return cb->commit(seq) && dev.wait(seq);
}

bool clearsTarget0(uint32_t value)
{
    return ((value >> 6) & 0xf) == 0 && (value & 0x3c);  // MRT_SELECT 0, a colour channel
}

void fetchBarriers(raw::Device &dev, Frame &frame, raw::Pipeline *writer, raw::Pipeline *fetching, Pushes &pushes)
{
    raw::CommandBuffer *cb = dev.begin();
    raw::RenderEncoder *e = cb ? cb->render(frame.pass(false)) : nullptr;
    bool drawn = e && frame.draw(e, writer, 0, 0, 64, 64);
    drawn = drawn && frame.draw(e, fetching, 128, 128, 64, 64);  // meets nothing written
    drawn = drawn && frame.draw(e, fetching, 32, 32, 64, 64);    // meets the first draw's pixels
    if (e)
        e->end();
    drawn = drawn && commit(dev, cb);
    const auto all = pushes.since();
    const unsigned elided = Pushes::count(all, 0, NVB197_NO_OPERATION, [](uint32_t v) {
        return v == kMarkElided;
    });
    const unsigned barriers = Pushes::count(all, 0, NVB197_PIXEL_SHADER_BARRIER);
    std::printf("      fetch barriers: %u taken, %u left out\n", barriers, elided);
    expect(drawn && elided == 1, "a fetching draw whose pixels meet no earlier write takes no barrier");
    expect(drawn && barriers == 1, "a fetching draw whose pixels meet an earlier draw's takes the barrier");
}

void clears(raw::Device &dev, Frame &frame, raw::Pipeline *opaque, Pushes &pushes)
{
    const auto pass = [&](uint32_t w, uint32_t h, unsigned &cleared) {
        raw::CommandBuffer *cb = dev.begin();
        raw::RenderEncoder *e = cb ? cb->render(frame.pass(true)) : nullptr;
        bool drawn = e && frame.draw(e, opaque, 0, 0, w, h);
        if (e)
            e->end();
        drawn = drawn && commit(dev, cb);
        cleared = Pushes::count(pushes.since(), 0, NVB197_CLEAR_SURFACE, clearsTarget0);
        return drawn;
    };
    unsigned whole = 0, half = 0;
    const bool covered = pass(kSize, kSize, whole), partial = pass(kSize, kSize / 2, half);
    std::printf("      target 0 clears: %u under a covering first draw, %u under a half one\n", whole, half);
    expect(covered && whole == 0, "a Clear under an opaque first draw over the whole target is left out");
    expect(partial && half >= 1, "a Clear under a first draw over half the target is kept");
}

// A split state of the pack whose colour targets this harness can make and that takes no depth
raw::Pipeline *splitState(raw::Device &dev)
{
    const raw::PackPipeline *recs = dev.pack().section<raw::PackPipeline>(raw::PackPipelines);
    for (uint32_t i = 0; i < dev.pack().header()->pipelines; i++) {
        raw::Pipeline *p = dev.pipeline(recs[i].keyHash);
        if (p && p->splitSource && !p->depthFormat && p->colorFormats[0] == kBGRA8 && !p->colorFormats[2] &&
            (!p->colorFormats[1] || p->colorFormats[1] == kBGRA8))
            return p;
    }
    return nullptr;
}

void splits(raw::Device &dev, Frame &frame, raw::Pipeline *split, Pushes &pushes)
{
    const auto pass = [&](uint32_t w, uint32_t h, unsigned &prologues) {
        constexpr uint32_t kDraws = 32;
        raw::CommandBuffer *cb = dev.begin();
        raw::RenderEncoder *e = cb ? cb->render(frame.pass(false)) : nullptr;
        if (e) {
            e->beginSplitPlan();
            e->addSplitCandidates(split, kDraws, uint64_t(w) * h);
            e->finishSplitPlan();
        }
        bool drawn = e != nullptr;
        for (uint32_t d = 0; drawn && d < kDraws; d++) {
            e->setPipeline(split);
            for (uint32_t k = 0; k < split->rec->entries; k++)
                if (const raw::PackEntry &x = split->rec->entry[k]; x.kind == raw::EntryUbo && x.stage < 3)
                    e->setBytes(x.stage, x.slot, frame.zeros, sizeof frame.zeros);
            e->setScissor(0, 0, w, h);
            const char *why = nullptr;
            drawn = e->draw(4, 0, 4, 1, 0, &why);
            if (!drawn)
                std::printf("      split draw refused: %s\n", why ? why : "-");
        }
        if (e)
            e->end();
        drawn = drawn && commit(dev, cb);
        prologues = Pushes::count(pushes.since(), raw::SUBC_COMPUTE, NVB1C0_SEND_PCAS_A);
        return drawn;
    };
    unsigned large = 0, small = 0;
    const bool wide = pass(kSize, kSize, large), narrow = pass(4, 4, small);
    std::printf("      split prologue launches: %u for large draws, %u for 4x4 ones\n", large, small);
    expect(wide && large >= 1, "a split state's large draws take its twin, the prologue launched before the pass");
    expect(narrow && small == 0, "a split state's draws of 16 pixels take the whole program, no prologue");
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: geomcheck <pack.bin> <corpus.log>\n");
        return 2;
    }
    char dump[] = "/tmp/geomcheck-XXXXXX";
    const int fd = mkstemp(dump);
    if (fd < 0)
        return 2;
    close(fd);
    setenv("PUSHDUMP", dump, 1);
    raw::Device dev;
    std::string err;
    if (!dev.open(argv[1], 0, err)) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return 1;
    }
    raw::Pipeline *writer = nullptr, *fetching = nullptr, *opaque = nullptr;
    for (const frame::State &s : frame::states(argv[2])) {
        if (!s.uber)
            continue;
        raw::Pipeline *p = dev.pipeline(raw::Pack::fnv(s.key.data(), s.key.size()), raw::PackForm::Default, true);
        if (!p || !p->positionComponents || p->colorFormats[0] != kBGRA8)
            continue;
        if (!writer && (p->writeBits & 2) && !p->fetchBits)
            writer = p;
        if (!fetching && (p->fetchBits & 2) && p->barrierTwin &&
            raw::fetchesOnlyUnwritten(p->fetchBits, p->writeBits, p->storageWriteBits, p->barrierTwin->fetchBits,
                                      p->barrierTwin->writeBits))
            fetching = p;
        if (!opaque && (p->writeBits & ~(p->readBits | p->fetchBits | p->storageBits) & 1) && !p->dropsPixels)
            opaque = p;
    }
    raw::Pipeline *split = splitState(dev);
    Frame frame{dev, {}, {}};
    if (!writer || !fetching || !opaque || !split ||
        !dev.createTexture(kSize, kSize, kBGRA8, frame.target, false, true) ||
        !dev.createMemoryless(kSize, kSize, kBGRA8, frame.scratch)) {
        std::fprintf(stderr, "no writer, fetching, opaque or split state (%p %p %p %p), or no target\n", (void *)writer,
                     (void *)fetching, (void *)opaque, (void *)split);
        return 1;
    }
    Pushes pushes;
    pushes.file = fopen(dump, "r");
    // the first command buffer's context and placement words are no pass's
    if (raw::CommandBuffer *cb = dev.begin())
        commit(dev, cb);
    pushes.since();
    fetchBarriers(dev, frame, writer, fetching, pushes);
    clears(dev, frame, opaque, pushes);
    splits(dev, frame, split, pushes);
    fclose(pushes.file);
    unlink(dump);
    std::printf("geomcheck: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
