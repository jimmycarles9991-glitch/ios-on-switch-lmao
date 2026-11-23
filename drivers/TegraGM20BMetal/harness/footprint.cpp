// SPDX-License-Identifier: MIT
// What the device costs a process, over the mock GPU: the bytes of kernel BOs it holds (allocations, and imports of its own
// memory) and of its malloc heap, at its open and after its first state, texture and command buffer. Every record of the pack is
// found by its key, in either record order (mkpack.py sorts them; an older pack goes through an index).
// usage: footprint <pack.bin> [client]   (MOCK_GPU_US=<n>: a client's push ring doubling under a slow GPU too)
#include <malloc/malloc.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "../gpu.h"

extern "C" void mock_iokit_held(uint64_t *allocated, uint64_t *imported);
extern "C" unsigned long mock_iokit_calls(unsigned method);

namespace {
int failures;

void expect(bool ok, const char *what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

uint64_t heapInUse()
{
    malloc_statistics_t s = {};
    malloc_zone_statistics(nullptr, &s);
    return s.size_in_use;
}

struct Sample {
    uint64_t allocated, imported, heap;
    unsigned long channels;
};

Sample sample()
{
    Sample s = {};
    mock_iokit_held(&s.allocated, &s.imported);
    s.heap = heapInUse();
    s.channels = mock_iokit_calls(TegraGM20BMethodChannelCreate);
    return s;
}

void report(const char *stage, const Sample &base, const Sample &now)
{
    std::printf("%-22s BOs %6llu KiB allocated, %6llu KiB imported; malloc +%llu KiB; channels %lu\n", stage,
                (unsigned long long)(now.allocated >> 10), (unsigned long long)(now.imported >> 10),
                (unsigned long long)((now.heap - base.heap) >> 10), now.channels);
}

bool everyRecordFound(const gm20b::Pack &pack)
{
    const gm20b::PackPipeline *p = pack.section<gm20b::PackPipeline>(gm20b::PackPipelines);
    for (uint32_t i = 0; i < pack.header()->pipelines; i++) {
        const gm20b::PackForm form = gm20b::recordForm(p[i]);
        const gm20b::PackPipeline *found = p[i].flags & gm20b::PipelineVariantRecord
                                               ? pack.variantRecord(p[i].keyHash, pack.string(p[i].variantName), form)
                                               : pack.find(p[i].keyHash, form);
        if (!found || found->keyHash != p[i].keyHash)
            return false;
    }
    return !pack.find(0x0123456789abcdefull);
}

// A client's push ring under command buffers of 2000 copies each while the GPU takes 2 ms a submission: it doubles where an
// allocation waited for the GPU. True: it grew, and the grown-out ring went back once the GPU passed it.
bool ringGrows(gm20b::Device &dev, const Sample &base)
{
    gm20b::Buffer a, b;
    if (!dev.createBuffer(1 << 16, a) || !dev.createBuffer(1 << 16, b))
        return false;
    const uint64_t ringsBefore = sample().allocated;
    uint64_t grew = 0;
    uint32_t seq = 0;
    for (int i = 0; i < 48; i++) {
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb)
            return false;
        grew = std::max(grew, sample().allocated - std::min(sample().allocated, ringsBefore));
        cb->use(a, false);
        cb->use(b, true, false);
        gm20b::BlitEncoder *be = cb->blit();
        for (int k = 0; k < 2000; k++)
            be->copyBytes(a.va, b.va, 64);
        be->end();
        if (!cb->commit(seq))
            return false;
    }
    dev.wait(seq);
    dev.begin()->commit(seq);
    dev.wait(seq);
    dev.begin()->commit(seq);
    report("after ring growth", base, sample());
    std::printf("push ring waits %llu, grew by %llu KiB\n", (unsigned long long)dev.stats.ringWaits.load(),
                (unsigned long long)(grew >> 10));
    dev.destroyBuffer(a);
    dev.destroyBuffer(b);
    return grew > 0;
}
}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: footprint <pack.bin> [client]\n");
        return 2;
    }
    const bool client = argc > 2 && !strcmp(argv[2], "client");
    const Sample base = sample();
    gm20b::Device dev;
    std::string err;
    if (!dev.open(argv[1], 0, err, client ? gm20b::Device::Role::Client : gm20b::Device::Role::RenderServer)) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return 1;
    }
    const Sample opened = sample();
    report("open", base, opened);
    expect(everyRecordFound(dev.pack()), "every record of the pack is found by its key");
    if (client)
        expect(!opened.channels && opened.allocated < (512 << 10) && !opened.imported,
               "a client's open makes no channel, heap or chunk (its header pools and tables alone)");
    const gm20b::PackPipeline *first = dev.pack().section<gm20b::PackPipeline>(gm20b::PackPipelines);
    expect(dev.pipeline(first->keyHash) != nullptr, "the first state is made");
    const Sample stated = sample();
    report("first state", base, stated);
    if (client)
        expect(stated.imported <= (64 << 10) && !stated.channels, "its programs bind a quarter heap step, no channel");
    gm20b::Texture t;
    dev.createTexture(256, 256, 80, t);
    const Sample textured = sample();
    report("first texture", base, textured);
    if (client)
        expect(textured.imported - stated.imported == (256u << 10),
               "a client's first texture wires one pool chunk of a sixteenth of the render server's");
    gm20b::CommandBuffer *cb = dev.begin();
    uint32_t seq = 0;
    const bool committed = cb && cb->commit(seq);
    const Sample submitted = sample();
    report("first command buffer", base, submitted);
    expect(committed && dev.wait(seq), "an empty command buffer runs");
    if (client)
        expect(submitted.channels == 1 && submitted.allocated - textured.allocated < (1 << 20),
               "a client's first command buffer makes its channel and rings (under 1 MiB)");
    dev.destroyTexture(t);
    dev.trim();
    const Sample trimmed = sample();
    report("trim", base, trimmed);
    expect(trimmed.imported == dev.heapBytes(), "a trim gives every empty chunk back (the heap's bound part stays)");
    if (client && getenv("MOCK_GPU_US"))
        expect(ringGrows(dev, base), "a ring that waited for the GPU doubles");
    if (failures) {
        std::printf("FAIL: footprint (%d)\n", failures);
        return 1;
    }
    std::printf("PASS: footprint\n");
    return 0;
}
