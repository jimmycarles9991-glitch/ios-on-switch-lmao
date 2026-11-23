// SPDX-License-Identifier: MIT
// raw GM20B path: an indexed draw past its buffer's indices (Metal reads index 0 there) over the mock GPU. The index
// fetch reads index 0 past its limit (caps row 3D-B11), so the draw binds its buffer, its limit at the buffer's fetch end,
// and the CPU copies and reads no index; a draw of no index the buffer holds reads the zero page under the same limit,
// longer than the page or not; a draw within the buffer binds it as it was.
// usage: indexcheck <pack.bin>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "../gpu.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B3D.h"

extern "C" uint32_t cap_reg3d(uint32_t mthd);

namespace {

int failures = 0;

void expect(bool ok, const char *what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

struct Bound {
    uint64_t start = 0, last = 0, uploaded = 0;
    bool drawn = false;
};

Bound drawIndexed(gm20b::Device &dev, gm20b::Pipeline *state, const gm20b::PassDesc &desc, const gm20b::Buffer &ib,
                  uint64_t offset, uint32_t count)
{
    Bound b;
    const uint64_t before = dev.stats.uploadBytes.load();
    gm20b::CommandBuffer *cb = dev.begin();
    gm20b::RenderEncoder *e = cb ? cb->render(desc) : nullptr;
    const char *why = nullptr;
    if (e) {
        e->setPipeline(state);
        b.drawn = e->drawIndexed(4, count, 2, &ib, offset, 1, 0, 0, &why);
        e->end();
    }
    uint32_t seq = 0;
    b.drawn = b.drawn && cb->commit(seq) && dev.wait(seq);
    if (!b.drawn)
        std::printf("      refused: %s\n", why ? why : "-");
    // the next command buffer's begin folds this one's counts into the device's
    if (gm20b::CommandBuffer *next = dev.begin())
        next->abandon();
    b.start = uint64_t(cap_reg3d(NVB197_SET_INDEX_BUFFER_A)) << 32 | cap_reg3d(NVB197_SET_INDEX_BUFFER_B);
    b.last = uint64_t(cap_reg3d(NVB197_SET_INDEX_BUFFER_C)) << 32 | cap_reg3d(NVB197_SET_INDEX_BUFFER_D);
    b.uploaded = dev.stats.uploadBytes.load() - before;
    if (getenv("INDEXCHECK_LOG"))
        std::printf("      buffer %llx: start %llx last %llx uploaded %llu\n", (unsigned long long)ib.va,
                    (unsigned long long)b.start, (unsigned long long)b.last, (unsigned long long)b.uploaded);
    return b;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: indexcheck <pack.bin>\n");
        return 2;
    }
    setenv("RAWCAP_LOG", "/dev/null", 0);  // the mock decodes the pushes, so cap_reg3d answers
    gm20b::Device dev;
    std::string err;
    if (!dev.open(argv[1], 0, err)) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return 1;
    }
    gm20b::Pipeline *state = nullptr;
    const gm20b::PackPipeline *recs = dev.pack().section<gm20b::PackPipeline>(gm20b::PackPipelines);
    for (uint32_t i = 0; i < dev.pack().header()->pipelines && !state; i++)
        if (gm20b::Pipeline *p = dev.pipeline(recs[i].keyHash);
            p && !p->depthFormat && !p->fetchBits && p->rec->vertexSlot < 31)
            state = p;
    gm20b::Texture targets[8];
    gm20b::PassDesc desc;
    for (uint32_t j = 0; state && j < 8; j++)
        if (const uint32_t f = state->colorFormats[j]) {
            if (!dev.createTexture(64, 64, f, targets[j]))
                return 1;
            desc.color[j].texture = &targets[j];
            desc.count = j + 1;
        }
    gm20b::Buffer ib;
    if (!state || !desc.count || !dev.createBuffer(10, ib, true)) {
        std::fprintf(stderr, "no state or index buffer\n");
        return 1;
    }
    const uint16_t indices[5] = {1, 2, 3, 4, 5};
    memcpy(ib.cpu, indices, sizeof indices);

    Bound inside = drawIndexed(dev, state, desc, ib, 0, 5);
    expect(inside.drawn && inside.start == ib.va && inside.last == ib.va + 9,
           "a draw of the buffer's own indices binds the buffer to its last index");

    Bound none = drawIndexed(dev, state, desc, ib, 16, 4);
    expect(none.drawn && (none.start < ib.va || none.start >= ib.va + ib.length) && none.last == none.start + 7 &&
               none.uploaded == inside.uploaded,
           "a draw of no index the buffer holds reads the zero page, nothing copied");

    Bound endless = drawIndexed(dev, state, desc, ib, 16, 1u << 20);
    expect(endless.drawn && endless.start == none.start && endless.last - endless.start < (1u << 21) - 1 &&
               endless.uploaded == inside.uploaded,
           "a draw of no index the buffer holds, longer than the zero page: the page under its own limit, nothing "
           "copied");

    Bound limited = drawIndexed(dev, state, desc, ib, 2, 8);
    expect(limited.drawn && limited.start == ib.va + 2 && limited.last == ib.va + 11 &&
               limited.uploaded == inside.uploaded,
           "past the buffer's end: the buffer bound at the draw's offset, its limit at the fetch end (12 bytes)");

    dev.destroyBuffer(ib);
    std::printf("indexcheck: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
