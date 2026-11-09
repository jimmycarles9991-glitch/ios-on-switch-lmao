// SPDX-License-Identifier: MIT
// MTLHeap arenas over the mock GPU (raw_arena.cpp): an arena holds its size from creation, places block-linear textures until
// full, takes a released range again, gives its pages back when purged with nothing committed (and not with something),
// comes back at the same GPU and CPU addresses (no texture header rewritten), and goes away whole when destroyed.
// usage: arenacheck <pack.bin>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>
#include "../runtime/raw_gpu.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: arenacheck <pack.bin>\n");
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
    const uint64_t before = dev.stats.poolBytes.load(), heap = 4ull << 20;
    uint32_t arena = 0;
    expect(dev.createArena(heap, true, arena), "a 4 MiB private heap's arena is made");
    expect(dev.stats.poolBytes.load() == before + heap, "it holds its size from the start");
    uint64_t bytes = 0, align = 0;
    expect(raw::Device::arenaFootprint(512, 512, 80, true, bytes, align) && bytes == 1u << 20 && align % 16384 == 0,
           "a 512x512 BGRA8 texture takes 1 MiB, 16 KiB aligned");
    std::vector<raw::Texture> textures(4);
    bool placed = true;
    for (raw::Texture &t : textures)
        placed &= dev.arenaTexture(arena, 512, 512, 80, t) && t.blockLinear && t.tic;
    expect(placed, "four block-linear 512x512 textures fill it");
    raw::Texture spare;
    expect(!dev.arenaTexture(arena, 512, 512, 80, spare), "a fifth is refused: Metal's full heap answers nil");
    expect(dev.arenaUsed(arena) == heap && dev.arenaLargestFree(arena, 16384) == 0, "usedSize is the heap's");
    const uint64_t va = textures[1].va;
    dev.arenaRelease(textures[1]);
    expect(dev.arenaTexture(arena, 512, 512, 80, spare) && spare.va == va,
           "an aliasable texture's range is placed again");

    std::atomic<uint32_t> committed{1};
    dev.setArenaVolatile(arena, true);
    dev.purgeArenas(committed);
    expect(!dev.arenaPurged(arena) && dev.purgedArenas() == 0, "nothing is purged while a command buffer is committed");
    committed = 0;
    dev.purgeArenas(committed);
    expect(dev.arenaPurged(arena) && dev.stats.poolBytes.load() == before, "a volatile heap's pages go back when idle");
    const uint64_t cpu0 = uint64_t(uintptr_t(textures[0].cpu));
    expect(dev.restoreArena(arena) && !dev.arenaPurged(arena) && dev.stats.poolBytes.load() == before + heap,
           "restored, it holds its size again");
    expect(textures[0].va == va - (1u << 20) && uint64_t(uintptr_t(textures[0].cpu)) == cpu0,
           "its textures keep their GPU and CPU addresses");

    for (raw::Texture &t : textures)
        dev.destroyTexture(t);
    dev.destroyTexture(spare);
    dev.destroyArena(arena);
    expect(dev.stats.poolBytes.load() == before, "destroyed, the heap's pages and VA go back");
    std::printf("%s\n", failures ? "arenacheck FAILED" : "arenacheck ok");
    return failures ? 1 : 0;
}
