// SPDX-License-Identifier: MIT
// raw GM20B path over the mock GPU, built with -fsanitize=thread: begin() and commit() take no device lock while other
// threads change what they read. The encoding thread's passes take memoryless backings and view headers of a texture it
// makes again every 16 frames, committing as exact jobs, while a second thread places the pack's states (the shader
// heap's parts and the local memory area move under the device's lock), makes and frees textures (header numbers come
// back with new generations) and trims as memory pressure does, and the compressed-memory thread trims backings. A
// texture's view header is its first ask's until it is freed, never its own header; once the churn stops, a command
// buffer takes no lock but the trim's once in 64.
// usage: MOCK_NATIVE_JOBS=1 MOCK_COMPRESSION=1 lockcheck <pack.bin> [frames]
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include "../runtime/raw_gpu.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: lockcheck <pack.bin> [frames]\n");
        return 2;
    }
    const unsigned frames = argc > 2 ? unsigned(atoi(argv[2])) : 400;
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
    constexpr uint32_t kBGRA8 = 80, kRGBA16Float = 115, kDepth32FloatStencil8 = 260, kR32Uint = 53;
    raw::Texture target, colour, zeta;
    if (!dev.createTexture(256, 256, kBGRA8, target) || !dev.createMemoryless(256, 256, kRGBA16Float, colour) ||
        !dev.createMemoryless(256, 256, kDepth32FloatStencil8, zeta)) {
        std::fprintf(stderr, "targets refused\n");
        return 1;
    }
    raw::Texture sampled;
    if (!dev.createTexture(64, 64, kBGRA8, sampled))
        return 1;
    const uint32_t word0 = 0x12345;  // a view's word 0: viewTic copies the rest of the header
    uint32_t firstView = dev.viewTic(sampled, word0, raw::ViewAccess::Texture);

    std::atomic<bool> churning{true};
    std::atomic<uint64_t> placed{0}, made{0};
    std::thread churn([&] {
        const raw::PackPipeline *recs = dev.pack().section<raw::PackPipeline>(raw::PackPipelines);
        const uint32_t count = dev.pack().header()->pipelines;
        std::vector<raw::Texture> textures(16);
        for (uint32_t i = 0; churning.load(std::memory_order_relaxed); i++) {
            if (i < count && dev.pipeline(recs[i].keyHash))
                placed.fetch_add(1, std::memory_order_relaxed);
            raw::Texture &t = textures[i % textures.size()];
            if (t.tic)
                dev.destroyTexture(t);
            if (dev.createTexture(32 + i % 64, 32, i % 2 ? kBGRA8 : kR32Uint, t))
                made.fetch_add(1, std::memory_order_relaxed);
            if (i % 97 == 0)
                dev.trim();
        }
        for (raw::Texture &t : textures)
            if (t.tic)
                dev.destroyTexture(t);
    });

    raw::PassDesc pd;
    pd.count = 2;
    pd.color[0].texture = &target;
    pd.color[0].load = raw::ColorAttachment::Clear;
    pd.color[1].texture = &colour;
    pd.color[1].load = raw::ColorAttachment::Clear;
    pd.color[1].store = false;
    pd.depthStencil.texture = &zeta;
    pd.depthStencil.clearDepth = pd.depthStencil.clearStencil = true;
    pd.depthStencil.depth = 1.0f;
    bool views = firstView && firstView != sampled.tic;
    unsigned framesMade = 0;
    auto frame = [&](raw::EncodeProfile *profile) {
        if (!profile && ++framesMade % 16 == 0) {
            dev.destroyTexture(sampled);
            views &= dev.createTexture(64, 64, kBGRA8, sampled);
            firstView = dev.viewTic(sampled, word0, raw::ViewAccess::Texture);
            views &= firstView && firstView != sampled.tic;
        }
        raw::tEncodeProfile = profile;
        raw::CommandBuffer *cb = dev.begin();
        bool ok = cb != nullptr;
        for (int pass = 0; ok && pass < 3; pass++) {
            raw::RenderEncoder *e = cb->render(pd);
            ok = e != nullptr;
            if (ok)
                e->end();
            views &= dev.viewTic(sampled, word0, raw::ViewAccess::Texture) == firstView;
        }
        uint32_t seq = 0;
        ok = ok && cb->commit(seq);
        raw::tEncodeProfile = nullptr;
        return ok && dev.wait(seq);
    };
    unsigned refused = 0;
    for (unsigned f = 0; f < frames; f++)
        refused += !frame(nullptr);
    churning.store(false, std::memory_order_relaxed);
    churn.join();
    std::printf("churn: %llu states placed, %llu textures made\n", (unsigned long long)placed.load(),
                (unsigned long long)made.load());
    expect(refused == 0, "every command buffer committed while the other threads churned");
    expect(views, "a texture's view header is its first ask's while it lives, never its own header");

    for (unsigned f = 0; f < 8; f++)
        frame(nullptr);
    raw::EncodeProfile steady;
    constexpr unsigned kSteady = 128;
    for (unsigned f = 0; f < kSteady; f++)
        refused += !frame(&steady);
    const uint64_t locks = steady.counts[size_t(raw::EncodeCount::Locks)];
    std::printf("steady: %u command buffers, %llu locks\n", kSteady, (unsigned long long)locks);
    expect(refused == 0, "every steady command buffer committed");
    expect(locks <= kSteady / 64, "a steady command buffer takes no device lock but the trim's once in 64");
    dev.destroyTexture(sampled);
    dev.destroyTexture(target);
    std::printf("%s\n", failures ? "lockcheck: FAIL" : "lockcheck: PASS");
    return failures ? 1 : 0;
}
