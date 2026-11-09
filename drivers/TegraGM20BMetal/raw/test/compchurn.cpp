// SPDX-License-Identifier: MIT
// raw GM20B path: compressed memoryless targets follow what a pass needs now, not every size it ever drew (raw_comp.h CompMem).
// Over the mock GPU with MOCK_COMPRESSION=1, passes of a memoryless RGBA16F colour and a memoryless D32S8 target in 24 sizes,
// sweep after sweep: one compressed buffer per tag and kind holds every size (boot 15: backboardd and iconservicesagent made
// one per size, 494 of them, until the GPU pool refused them), nothing is refused, nothing grows from sweep to sweep, and
// with no command buffer after the last one the compressed memory goes back to the kernel (an idle process kept all of it).
// usage: compchurn <pack.bin> [sweeps]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include "../runtime/raw_gpu.h"

extern "C" void mock_iokit_comp_stats(unsigned long out[5]);

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: compchurn <pack.bin> [sweeps]\n");
        return 2;
    }
    const unsigned sweeps = argc > 2 ? unsigned(atoi(argv[2])) : 6;
    raw::Device dev;
    std::string err;
    if (!dev.open(argv[1], 0, err)) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return 1;
    }
    int failures = 0;
    auto expect = [&](bool ok, const char *what) {
        std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
        failures += !ok;
    };
    constexpr uint32_t kRGBA16Float = 115, kDepth32FloatStencil8 = 260;

    struct Target {
        raw::Texture colour, zeta;
    };

    std::vector<Target> targets(24);
    for (size_t i = 0; i < targets.size(); i++) {
        const uint32_t w = 720 - uint32_t(i) * 27, h = 1280 - uint32_t(i) * 50;
        if (!dev.createMemoryless(w, h, kRGBA16Float, targets[i].colour) ||
            !dev.createMemoryless(w, h, kDepth32FloatStencil8, targets[i].zeta)) {
            std::fprintf(stderr, "memoryless target %zu refused\n", i);
            return 1;
        }
    }
    auto pass = [&](Target &t) {
        raw::CommandBuffer *cb = dev.begin();
        raw::PassDesc pd;
        pd.count = 1;
        pd.color[0].texture = &t.colour;
        pd.color[0].load = raw::ColorAttachment::Clear;
        pd.color[0].store = false;
        pd.depthStencil.texture = &t.zeta;
        pd.depthStencil.clearDepth = pd.depthStencil.clearStencil = true;
        pd.depthStencil.depth = 1.0f;
        raw::RenderEncoder *e = cb ? cb->render(pd) : nullptr;
        if (!e)
            return false;
        e->end();
        uint32_t seq = 0;
        return cb->commit(seq) && dev.wait(seq);
    };
    // the largest first, as a full-screen layer comes before the smaller ones: its ask covers theirs
    for (Target &t : targets)
        if (!pass(t)) {
            std::fprintf(stderr, "pass refused\n");
            return 1;
        }
    for (int i = 0; i < 200 && dev.stats.compMade.load() < 2; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    uint64_t held = 0;
    for (unsigned s = 1; s < sweeps; s++) {
        for (size_t i = 0; i < targets.size(); i++)
            if (!pass(targets[(i * 7 + s) % targets.size()])) {
                std::fprintf(stderr, "pass refused\n");
                return 1;
            }
        std::printf("sweep %u: held %llu (%llu KiB) views %llu made %llu refused %llu\n", s,
                    (unsigned long long)dev.stats.compHeld.load(),
                    (unsigned long long)(dev.stats.compHeldBytes.load() >> 10),
                    (unsigned long long)dev.stats.compViews.load(), (unsigned long long)dev.stats.compMade.load(),
                    (unsigned long long)dev.stats.compFailed.load());
        if (s == 1)
            held = dev.stats.compHeldBytes.load();
        else if (dev.stats.compHeldBytes.load() != held)
            held = ~0ull;
    }
    expect(dev.stats.compMade.load() == 2, "one compressed buffer for the colour targets and one for the zeta targets");
    expect(dev.stats.compFailed.load() == 0, "no compressed memory refused");
    expect(held != ~0ull, "the compressed memory held the same after every sweep");
    expect(dev.stats.compViews.load() == 2 * targets.size(), "a compressed backing per size and tag");
    std::printf("idle for %llu s\n", (unsigned long long)(raw::kCompIdleNs / 1'000'000'000 + 2));
    std::this_thread::sleep_for(std::chrono::nanoseconds(raw::kCompIdleNs + 2'000'000'000));
    std::printf("idle: held %llu (%llu KiB) views %llu\n", (unsigned long long)dev.stats.compHeld.load(),
                (unsigned long long)(dev.stats.compHeldBytes.load() >> 10),
                (unsigned long long)dev.stats.compViews.load());
    expect(dev.stats.compHeld.load() == 0 && dev.stats.compHeldBytes.load() == 0,
           "the compressed memory went back once the device was idle");
    expect(dev.stats.compViews.load() == 0, "no compressed backing left");
    unsigned long m[5];
    mock_iokit_comp_stats(m);
    std::printf("kernel: binds %lu refused %lu alias %lu cpu %lu nolines %lu\n", m[0], m[1], m[2], m[3], m[4]);
    expect(m[1] == 0 && m[2] == 0 && m[3] == 0, "the kernel's compression contract kept (refused, alias, CPU 0)");
    if (!pass(targets[0]))
        return 1;
    for (int i = 0; i < 200 && dev.stats.compHeld.load() < 1; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!pass(targets[0]))
        return 1;
    expect(dev.stats.compViews.load() >= 1, "compression comes back for the next pass after the idle trim");
    std::printf("%s\n", failures ? "compchurn: FAIL" : "compchurn: PASS");
    return failures ? 1 : 0;
}
