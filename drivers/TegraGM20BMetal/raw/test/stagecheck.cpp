// SPDX-License-Identifier: MIT
// stagecheck: a pack state's set 0 and constant buffers as the raw render encoder binds them, over the mock GPU. Every
// buffer slot of both stages takes a buffer of its own, and every texture and sampler slot the record's handles name, at
// its stage, a texture and sampler of its own. Each handle word must name the header of what is bound at its entry's stage
// and slot, and each uniform buffer a program reads as a constant buffer must be bound, at that constant buffer slot of
// its stage's group, to the buffer of the entry's stage and slot (the decoder's binds after the draw); stagecheck.py then
// runs the state's programs over these words and over the words m2v's binding of the same slots makes.
// usage: stagecheck <pack.bin> <key hash, hex> <out.json> [swap|unbind]
//   swap, unbind: the check's own controls, each of which it must fail: the vertex program's last uniform buffer
//   constant buffer's entry takes another slot's buffer, or none
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include "../runtime/raw_gpu.h"

extern "C" bool cap_cbuf(int group, int slot, uint64_t *va, uint32_t *bytes);

namespace raw {
struct EncoderHarness {
    static const DescriptorWords &setWords(const RenderEncoder &e)
    {
        return e.descriptors_.last().words;
    }
};
}  // namespace raw

namespace {

using Slot = std::pair<uint32_t, uint32_t>;  // stage (1 vertex, 2 fragment), slot

bool handle(const raw::PackEntry &e)
{
    return e.kind == raw::EntryTexture || e.kind == raw::EntryTexture3d || e.kind == raw::EntrySamplerOnly;
}

// The uniform buffers `program` reads as constant buffers, each bound at its slot of group `group` to the buffer of the
// set-0 entry its type names; prints each one that is not. Returns {checked, wrong}.
std::pair<unsigned, unsigned> checkUniforms(const raw::PackProgram &program, int group, const raw::PackPipeline &r,
                                            const std::map<Slot, raw::Buffer> &buffers)
{
    unsigned checked = 0, wrong = 0;
    for (uint32_t c = 1; c < program.cbufs; c++) {
        if (raw::cbufKind(program.cbufType[c]) != raw::CbufUbo)
            continue;
        const uint32_t offset = raw::cbufUboOffset(program.cbufType[c]);
        const raw::PackEntry *named = nullptr;
        for (uint32_t i = 0; i < r.entries && !named; i++)
            if (r.entry[i].kind == raw::EntryUbo && !raw::bufferStorage(r.entry[i]) && r.entry[i].offset == offset)
                named = &r.entry[i];
        const auto bound = named ? buffers.find({named->stage, named->slot}) : buffers.end();
        uint64_t va = 0;
        uint32_t bytes = 0;
        const bool held = cap_cbuf(group, int(c), &va, &bytes);
        ++checked;
        if (bound == buffers.end() || !held || va != bound->second.va || !bytes) {
            ++wrong;
            printf("WRONG cbuf %u of group %d: set-0 offset %u, entry %s, bound %s %llx\n", c, group, offset,
                   named ? (std::to_string(named->stage) + ":" + std::to_string(named->slot)).c_str() : "none",
                   held ? "at" : "nothing", (unsigned long long)va);
        }
    }
    return {checked, wrong};
}

// The stage and slot of the set-0 entry the vertex program's last uniform buffer constant buffer names ({0, 0}: none)
Slot lastVertexUniform(const raw::Device &dev, const raw::PackPipeline &r)
{
    const raw::PackProgram &vs = dev.pack().section<raw::PackProgram>(raw::PackPrograms)[r.vs];
    Slot found{0, 0};
    for (uint32_t c = 1; c < vs.cbufs; c++)
        if (raw::cbufKind(vs.cbufType[c]) == raw::CbufUbo)
            for (uint32_t i = 0; i < r.entries; i++)
                if (r.entry[i].kind == raw::EntryUbo && r.entry[i].offset == raw::cbufUboOffset(vs.cbufType[c]))
                    found = {r.entry[i].stage, r.entry[i].slot};
    return found;
}

}  // namespace

int main(int argc, char **argv)
{
    setenv("RAWCAP_LOG", "/dev/null", 0);  // the decoder runs only with a log, and cap_cbuf reads its binds
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: stagecheck <pack.bin> <key hash, hex> <out.json> [swap|unbind]\n");
        return 2;
    }
    raw::Device dev;
    std::string err;
    if (!dev.open(argv[1], 0, err)) {
        fprintf(stderr, "raw device: %s\n", err.c_str());
        return 1;
    }
    const uint64_t key = strtoull(argv[2], nullptr, 16);
    raw::Pipeline *pl = dev.pipeline(key);
    if (!pl) {
        fprintf(stderr, "%016llx: not in the pack\n", (unsigned long long)key);
        return 1;
    }
    const raw::PackPipeline &r = *pl->rec;
    std::map<Slot, raw::Buffer> buffers;
    for (uint32_t stage = 1; stage <= 2; stage++)
        for (uint32_t slot = 0; slot < 31; slot++)
            if (!dev.createBuffer(4096, buffers[{stage, slot}]))
                return 1;
    std::map<Slot, raw::Texture> textures;
    std::map<Slot, raw::Sampler> samplers;
    for (uint32_t i = 0; i < r.entries; i++) {
        const raw::PackEntry &e = r.entry[i];
        if (!handle(e))
            continue;
        const Slot at{e.stage, e.slot};
        if (e.kind != raw::EntrySamplerOnly && !textures.count(at)) {
            raw::Texture &t = textures[at];
            const uint32_t size = 8 + 2 * e.slot + 64 * (e.stage - 1);
            const bool made = e.kind == raw::EntryTexture3d ? dev.createVolume(size, size, 4, 80, false, t)
                                                            : dev.createTexture(size, size, 80, t);
            if (!made) {
                fprintf(stderr, "texture %u:%u\n", e.stage, e.slot);
                return 1;
            }
        }
        if (e.samplerKind == raw::SamplerSlot && !samplers.count({e.stage, e.sampler})) {
            raw::SamplerDesc d;
            d.lodMin = float(16 * e.stage + e.sampler);
            if (!dev.createSampler(d, samplers[{e.stage, e.sampler}]))
                return 1;
        }
    }
    std::vector<raw::Texture> targets(9);
    raw::PassDesc pass;
    for (uint32_t j = 0; j < 8; j++) {
        if (!pl->colorFormats[j])
            continue;
        const bool made = pl->samples > 1
                              ? dev.createStoredMultisample(16, 16, pl->colorFormats[j], pl->samples, targets[j])
                              : dev.createTexture(16, 16, pl->colorFormats[j], targets[j], false, true);
        if (!made) {
            fprintf(stderr, "color %u: format %u at %u samples\n", j, pl->colorFormats[j], pl->samples);
            return 1;
        }
        pass.color[j].texture = &targets[j];
        pass.color[j].store = true;
        pass.count = j + 1;
    }
    if (pl->depthFormat) {
        if (!dev.createOwnedZeta(16, 16, pl->depthFormat, pl->samples, targets[8])) {
            fprintf(stderr, "depth: format %u at %u samples\n", pl->depthFormat, pl->samples);
            return 1;
        }
        pass.depthStencil.texture = &targets[8];
    }
    raw::CommandBuffer *cb = dev.begin();
    raw::RenderEncoder *enc = cb ? cb->render(pass) : nullptr;
    if (!enc) {
        fprintf(stderr, "render pass: %s\n", dev.channelError().c_str());
        return 1;
    }
    enc->setPipeline(pl);
    enc->setScissor(0, 0, 16, 16);
    const std::string control = argc == 5 ? argv[4] : "";
    const Slot controlled = lastVertexUniform(dev, r);
    if (!control.empty())
        printf("control %s: vertex uniform entry at stage %u slot %u\n", control.c_str(), controlled.first,
               controlled.second);
    for (auto &[at, b] : buffers) {
        if (at == controlled && control == "unbind")
            continue;
        const bool swap = at == controlled && control == "swap";
        enc->setBuffer(int(at.first), at.second, swap ? &buffers.at({at.first, (at.second + 1) % 31}) : &b, 0);
    }
    for (auto &[at, t] : textures)
        enc->setTexture(int(at.first), at.second, &t);
    for (auto &[at, s] : samplers)
        enc->setSampler(int(at.first), at.second, &s);
    const char *why = nullptr;
    if (!enc->draw(3, 0, 3, 1, 0, &why)) {
        fprintf(stderr, "draw: %s\n", why ? why : "refused");
        return 1;
    }
    const raw::DescriptorWords &words = raw::EncoderHarness::setWords(*enc);
    unsigned wrong = 0, checked = 0;
    FILE *out = fopen(argv[3], "w");
    if (!out)
        return 1;
    fprintf(out, "{\"key\": \"%016llx\", \"entries\": [", (unsigned long long)key);
    for (uint32_t i = 0; i < r.entries; i++) {
        const raw::PackEntry &e = r.entry[i];
        uint32_t want = 0;
        bool sampled = false;
        if (handle(e)) {
            const auto t = textures.find({e.stage, e.slot});
            const uint32_t tic = t == textures.end() ? 0 : t->second.tic;
            const auto s = samplers.find({e.stage, e.sampler});
            want = tic | (e.samplerKind == raw::SamplerSlot && s != samplers.end() ? s->second.tsc << 20 : 0);
            sampled = true;
            const uint32_t got = words[e.offset / 4];
            const uint32_t mask = e.samplerKind == raw::SamplerConstexpr ? 0xFFFFFu : ~0u;
            ++checked;
            if ((got & mask) != want) {
                ++wrong;
                printf("WRONG %u: stage %u slot %u sampler %u at %u: %08x, bound %08x\n", i, e.stage, e.slot, e.sampler,
                       e.offset, got, want);
            }
        }
        fprintf(out, "%s[%u, %u, %u, %u, %u, %u, %u]", i ? ", " : "", e.offset, e.kind, e.stage, e.slot, e.samplerKind,
                e.sampler, sampled ? words[e.offset / 4] : 0);
    }
    fprintf(out, "], \"textures\": [");
    const char *sep = "";
    for (auto &[at, t] : textures) {
        fprintf(out, "%s[%u, %u, %u]", sep, at.first, at.second, t.tic);
        sep = ", ";
    }
    fprintf(out, "], \"samplers\": [");
    sep = "";
    for (auto &[at, smp] : samplers) {
        fprintf(out, "%s[%u, %u, %u]", sep, at.first, at.second, smp.tsc);
        sep = ", ";
    }
    fprintf(out, "], \"words\": [");
    for (uint32_t i = 0; i < words.size(); i++)
        fprintf(out, "%s%u", i ? ", " : "", words[i]);
    fprintf(out, "]}\n");
    fclose(out);
    enc->end();
    uint32_t sequence = 0;
    const bool committed = cb->commit(sequence) && dev.wait(sequence);
    const raw::PackProgram *programs = dev.pack().section<raw::PackProgram>(raw::PackPrograms);
    const auto [vsChecked, vsWrong] = checkUniforms(programs[r.vs], 0, r, buffers);
    const auto [fsChecked, fsWrong] = checkUniforms(programs[r.fs], 4, r, buffers);
    printf("%016llx: %u handles, %u not what their stage and slot bind; %u + %u uniform buffers (vertex, pixel), %u "
           "not their own; %s\n",
           (unsigned long long)key, checked, wrong, vsChecked, fsChecked, vsWrong + fsWrong,
           committed ? "drawn" : dev.channelError().c_str());
    return wrong || vsWrong || fsWrong || !committed ? 1 : 0;
}
