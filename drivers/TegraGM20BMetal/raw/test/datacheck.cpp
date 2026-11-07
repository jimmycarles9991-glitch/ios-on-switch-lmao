// SPDX-License-Identifier: MIT
// raw GM20B path: one raw draw of every pack state whose programs read NAK's shader data, over the mock GPU (cap.cpp decodes it
// when RAWCAP_LOG is set), with the probe's tagged buffers (pack/probe.mm), so test/datacheck.py can hold each stage's constant
// buffer bindings against what the probe's NVK twin drew for the same state: the program, its shader data's slot, size and
// bytes, and set 0's slot. Prints "DATACHECK <key hash> <key>" per state, in draw order.
// usage: datacheck <pack.bin>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../runtime/raw_gpu.h"

namespace {

bool readsShaderData(const raw::Pack &pack, const raw::PackPipeline &p)
{
    const raw::PackProgram *progs = pack.section<raw::PackProgram>(raw::PackPrograms);
    return progs[p.vs].dataBytes || progs[p.fs].dataBytes;
}

// The color formats of a pack key's "c<i>=<format>:<writeMask>:" fields, and which of them are written
uint32_t colorFormats(const char *key, uint32_t formats[8], bool written[8])
{
    uint32_t count = 0;
    for (const char *at = key; (at = strstr(at, " c")); at += 2) {
        unsigned c = 0, f = 0, mask = 0;
        if (sscanf(at, " c%u=%u:%u:", &c, &f, &mask) == 3 && c < 8) {
            formats[c] = f;
            written[c] = mask != 0;
            count = std::max(count, c + 1);
        }
    }
    return count;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: datacheck <pack.bin>\n");
        return 2;
    }
    raw::Device dev;
    std::string err;
    if (!dev.open(argv[1], 0, err)) {
        fprintf(stderr, "raw device: %s\n", err.c_str());
        return 1;
    }
    const raw::Pack &pack = dev.pack();
    const raw::PackPipeline *pipes = pack.section<raw::PackPipeline>(raw::PackPipelines);
    raw::Texture textures[16];
    for (uint32_t t = 0; t < 16; t++)
        if (!dev.createTexture(16 + t, 8 + t, 80, textures[t])) {
            fprintf(stderr, "texture %u failed\n", t);
            return 1;
        }
    raw::Sampler samplers[16];
    for (uint32_t s = 0; s < 16; s++) {
        raw::SamplerDesc d;
        d.minFilter = d.magFilter = s & 1;
        d.mipFilter = 1;
        d.sAddress = s & 2 ? 2 : 0;
        d.lodMax = 1 + s / 2;
        dev.createSampler(d, samplers[s]);
    }
    unsigned drawn = 0;
    for (uint32_t i = 0; i < pack.header()->pipelines; i++) {
        const raw::PackPipeline &p = pipes[i];
        if (!readsShaderData(pack, p) || (p.flags & raw::PipelineBarrierForm))
            continue;
        raw::Pipeline *pl = dev.pipeline(p.keyHash);
        if (!pl) {
            fprintf(stderr, "not made: %s\n", pack.string(p.keyString));
            return 1;
        }
        uint32_t formats[8] = {};
        bool written[8] = {};
        const char *key = pack.string(p.keyString);
        raw::PassDesc pd;
        pd.count = colorFormats(key, formats, written);
        pd.width = pd.height = 64;
        std::vector<raw::Texture> targets(pd.count);
        for (uint32_t c = 0; c < pd.count; c++) {
            const bool made = written[c] ? dev.createTexture(64, 64, formats[c], targets[c])
                                         : dev.createMemoryless(64, 64, formats[c], targets[c]);
            if (!made) {
                fprintf(stderr, "target %u (format %u) failed: %s\n", c, formats[c], key);
                return 1;
            }
            pd.color[c].texture = &targets[c];
            pd.color[c].load = written[c] ? raw::ColorAttachment::Load : raw::ColorAttachment::DontCare;
            pd.color[c].store = written[c];
        }
        raw::CommandBuffer *cb = dev.begin();
        raw::RenderEncoder *e = cb->render(pd);
        if (!e) {
            fprintf(stderr, "no pass for %s\n", key);
            return 1;
        }
        e->setPipeline(pl);
        std::vector<uint32_t> tag(64);
        for (int stage = 1; stage <= 2; stage++)
            for (uint32_t slot = 0; slot < 8; slot++) {
                tag[0] = 0xC0DE0000u | uint32_t(stage) << 8 | slot;
                tag[1] = drawn;
                for (uint32_t k = 2; k < tag.size(); k++)
                    tag[k] = 0xA0000000u | uint32_t(stage) << 20 | slot << 16 | k;
                e->setBytes(stage, slot, tag.data(), uint32_t(tag.size() * 4));
            }
        for (uint32_t t = 0; t < 16; t++) {
            e->setTexture(t, &textures[t]);
            e->setSampler(t, &samplers[t]);
        }
        const char *why = nullptr;
        if (!e->draw(4, 0, 4, 1, 0, &why)) {
            fprintf(stderr, "draw refused (%s): %s\n", why ? why : "?", key);
            return 1;
        }
        e->end();
        uint32_t seq = 0;
        if (!cb->commit(seq) || !dev.wait(seq)) {
            fprintf(stderr, "commit failed\n");
            return 1;
        }
        printf("DATACHECK %016llx %s\n", (unsigned long long)p.keyHash, key);
        for (raw::Texture &t : targets)
            dev.destroyTexture(t);
        drawn++;
    }
    fprintf(stderr, "datacheck: %u states drawn\n", drawn);
    return drawn ? 0 : 1;
}
