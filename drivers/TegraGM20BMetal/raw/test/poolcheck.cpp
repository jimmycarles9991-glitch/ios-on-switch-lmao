// SPDX-License-Identifier: MIT
// raw GM20B path (r8-rawca, r8a): the raw device's memory follows what Core Animation holds, not its peak. A burst of shared
// buffers (write-combined GPU-pool chunks, r17) and textures (pool chunks) is made and freed, over the mock GPU; after it the
// device keeps one empty write-combined chunk and a chunk's worth of free pool memory, not the burst's (r8a: the compositor
// held 107 MiB of the GPU pool at 15:16 and 63 after, the raw pool 320 MB in 20 chunks to the end, and Maps was refused
// a 4 MiB BO with 33 MiB of the pool free). Then a burst of block-linear images (private textures; a pass's memoryless backings
// take the same chunks at commit) with the grower running: its chunks are made ahead of need, so no allocation wires one
// itself, and go back after it (boot 15: backboardd's grew 356 -> 953 MB in 64 imports on the commit path and stayed). Then
// passes whose memoryless attachment takes a new size each frame (an animated layer, as boot 15's keyboard drew): each
// size's backing is a view of its attachment's plain memory, taken in size classes, so a growing burst holds about its
// largest class, a shrinking one takes none, and an idle process gives it all back after 10 s.
// usage: poolcheck <pack.bin>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "../runtime/raw_gpu.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: poolcheck <pack.bin>\n");
        return 2;
    }
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
    const uint64_t poolBefore = dev.stats.poolBytes.load();
    // the burst: 48 MiB of shared buffers (Core Animation's buffer pool at a busy screen) and 96 MiB of textures
    std::vector<raw::Buffer> buffers(192);
    unsigned wc = 0;
    for (raw::Buffer &b : buffers)
        wc += dev.createBuffer(256 << 10, b, true) && b.wc;
    std::vector<raw::Texture> textures(24);
    unsigned made = 0;
    for (raw::Texture &t : textures)
        made += dev.createTexture(1024, 1024, 80, t);  // 4 MiB each
    const uint64_t wcPeak = dev.stats.wcBytes.load(), poolPeak = dev.stats.poolBytes.load();
    std::printf("burst: %u write-combined buffers (%llu KiB), %u textures, pool %llu -> %llu MiB\n", wc,
                (unsigned long long)(wcPeak >> 10), made, (unsigned long long)(poolBefore >> 20),
                (unsigned long long)(poolPeak >> 20));
    expect(wc == buffers.size() && made == textures.size(), "the burst was made");
    for (raw::Buffer &b : buffers)
        dev.destroyBuffer(b);
    for (raw::Texture &t : textures)
        dev.destroyTexture(t);
    const uint64_t poolAfter = dev.stats.poolBytes.load();
    std::printf("after: write-combined %llu KiB in buffers, %u chunks (%llu MiB) held; pool %llu MiB\n",
                (unsigned long long)(dev.stats.wcBytes.load() >> 10), dev.wcChunksHeld(),
                (unsigned long long)(dev.wcChunkBytesHeld() >> 20), (unsigned long long)(poolAfter >> 20));
    expect(dev.stats.wcBytes.load() == 0, "no write-combined buffer bytes left");
    expect(dev.wcChunksHeld() <= 1, "at most one empty write-combined chunk kept (was: every chunk of the burst)");
    expect(poolAfter <= poolBefore + (32ull << 20),
           "the pool back to a chunk's worth past its start (was: the burst's)");
    // and it grows again for the next burst
    raw::Buffer again;
    expect(dev.createBuffer(256 << 10, again, true) && again.wc, "a shared buffer after the trim is write-combined");
    dev.destroyBuffer(again);

    dev.warm();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // the grower's start
    const uint64_t tiledBefore = dev.stats.poolBytes.load(), stallsBefore = dev.stats.poolStalls.load();
    expect(dev.blChunkFreeBytes() > (16ull << 20), "warm() makes the block-linear chunk ahead of the first");
    std::vector<raw::Texture> images(64);
    unsigned imagesMade = 0;
    for (raw::Texture &t : images) {
        imagesMade += dev.createTexture(1024, 512, 80, t, false, true) && t.blockLinear;  // 2 MiB each
        std::this_thread::sleep_for(std::chrono::milliseconds(2));                        // a frame: the grower's turn
    }
    const uint64_t tiledPeak = dev.stats.poolBytes.load(), stalls = dev.stats.poolStalls.load() - stallsBefore,
                   ahead = dev.blChunkFreeBytes();
    std::printf(
        "block-linear burst: %u images, pool %llu -> %llu MiB, %llu MiB free after it, %llu allocations wired a "
        "chunk themselves\n",
        imagesMade, (unsigned long long)(tiledBefore >> 20), (unsigned long long)(tiledPeak >> 20),
        (unsigned long long)(ahead >> 20), (unsigned long long)stalls);
    expect(imagesMade == images.size(), "the block-linear burst was made");
    expect(ahead >= (16ull << 20), "a block-linear chunk's worth kept free ahead of need (was: none, grown when full)");
    expect(stalls == 0, "no block-linear allocation wired a chunk itself");
    for (raw::Texture &t : images)
        dev.destroyTexture(t);
    const uint64_t tiledAfter = dev.stats.poolBytes.load();
    std::printf("after: pool %llu MiB\n", (unsigned long long)(tiledAfter >> 20));
    expect(tiledAfter <= tiledBefore + (32ull << 20),
           "the block-linear chunks back to a chunk's worth past the start (was: the burst's, for good)");
    raw::Texture target;
    expect(dev.createTexture(1024, 1024, 80, target, false, true) && target.blockLinear, "a block-linear target");
    uint32_t seq = 0;
    auto frames = [&](int count, int width, int height, int step) {
        for (int i = 0; i < count; i++) {
            raw::Texture scratch;
            if (!dev.createMemoryless(uint32_t(width + step * 8 * i), uint32_t(height + step * 5 * i), 115, scratch, 1))
                return false;
            raw::CommandBuffer *cb = dev.begin();
            raw::PassDesc p;
            p.color[0].texture = &target;
            p.color[1].texture = &scratch;
            p.color[1].load = raw::ColorAttachment::Clear;
            p.color[1].store = false;
            p.count = 2;
            raw::RenderEncoder *e = cb ? cb->render(p) : nullptr;
            if (!e)
                return false;
            e->end();
            if (!cb->commit(seq))
                return false;
            dev.destroyTexture(scratch);
        }
        dev.wait(seq);
        return true;
    };
    const uint64_t memBefore = dev.stats.backingBytes.load(), poolBefore2 = dev.stats.poolBytes.load();
    expect(frames(48, 320, 180, 1), "48 frames growing 8 x 5 pixels each");
    const uint64_t grown = dev.stats.backingBytes.load() - memBefore;
    expect(frames(48, 692, 411, -1), "48 frames shrinking through other sizes");
    const uint64_t shrunk = dev.stats.backingBytes.load() - memBefore,
                   chunks = dev.stats.poolBytes.load() - poolBefore2;
    std::printf("memoryless: growing held %llu KiB of backing memory, shrinking %llu KiB; chunks grew %llu MiB\n",
                (unsigned long long)(grown >> 10), (unsigned long long)(shrunk >> 10),
                (unsigned long long)(chunks >> 20));
    expect(grown <= (8ull << 20), "a growing burst holds a few size classes of its largest frame (2.3 MiB), not each");
    std::this_thread::sleep_for(std::chrono::seconds(2));  // the backings' thread frees what retired once it completed
    std::printf("2 s on: %llu KiB of backing memory\n", (unsigned long long)(dev.stats.backingBytes.load() >> 10));
    expect(dev.stats.backingBytes.load() - memBefore <= (4ull << 20), "then only its largest frame's class");
    expect(shrunk <= grown, "a shrinking burst takes no memory (was: one backing a frame)");
    expect(chunks <= (48ull << 20), "the two bursts' backings take at most 48 MiB of chunks (was: 144)");
    std::this_thread::sleep_for(std::chrono::seconds(12));
    std::printf("12 s idle: %llu KiB of backing memory, %llu backings\n",
                (unsigned long long)(dev.stats.backingBytes.load() >> 10),
                (unsigned long long)dev.stats.backings.load());
    expect(dev.stats.backingBytes.load() == memBefore,
           "an idle process gives its backings' memory back after 10 s (was: at its 600th command buffer)");
    dev.destroyTexture(target);
    if (failures) {
        std::printf("FAIL: poolcheck (%d)\n", failures);
        return 1;
    }
    std::printf("PASS: poolcheck\n");
    return 0;
}
