// SPDX-License-Identifier: MIT
// raw GM20B path, over the mock GPU, built with -fsanitize=thread: what the completion queue adds to the device while the
// submit thread draws. addRecord places a runtime record's constexpr samplers after the pack's (Device::constexprTsc_), and
// a draw reads its own's slot without the device lock: two hundred records, each with a constexpr sampler of its own, are made
// on one thread while another draws one made before them.
// usage: racecheck <pack.bin>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include "../runtime/raw_gpu.h"
#include "../runtime/raw_record.h"

namespace {

// A runtime record like pack state `like`, under a key of its own, its programs placeholders (the mock runs none), sampling a
// texture through a constexpr sampler the pack lacks
raw::RuntimeRecord record(const raw::Device &dev, const raw::Pipeline &like, uint32_t n)
{
    raw::RuntimeRecord r;
    r.pipeline = *like.rec;
    const std::string key = "racecheck " + std::to_string(n);
    r.pipeline.keyHash = raw::Pack::fnv(key.data(), key.size());
    r.keyString = dev.pack().string(like.rec->keyString);
    const uint32_t *blob = dev.pack().section<uint32_t>(raw::PackBlob) + like.rec->blob;
    r.blob.assign(blob, blob + like.rec->blobDwords);
    for (raw::CapturedProgram &p : r.programs) {
        p.id = key + (&p == r.programs ? " vertex" : " fragment");
        p.image.assign(96, 0);
    }
    raw::PackEntry &e = r.pipeline.entry[r.pipeline.entries < 16 ? r.pipeline.entries++ : 15];
    e = {0xf0, raw::EntryTexture, 2, 0, raw::SamplerConstexpr, 0, 0};
    r.samplers.push_back({0x26092, 0x291, 0xfff000, n, 0, 0, 0, 0});
    return r;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: racecheck <pack.bin>\n");
        return 2;
    }
    raw::Device dev;
    std::string err;
    if (!dev.open(argv[1], 0, err)) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return 1;
    }
    const raw::PackPipeline *recs = dev.pack().section<raw::PackPipeline>(raw::PackPipelines);
    raw::Pipeline *like = nullptr;
    for (uint32_t i = 0; i < dev.pack().header()->pipelines && !like; i++)
        if (raw::Pipeline *p = dev.pipeline(recs[i].keyHash); p && !p->depthFormat && p->rec->entries < 16)
            like = p;
    raw::Texture targets[8];
    raw::PassDesc pass;
    for (uint32_t j = 0; like && j < 8; j++)
        if (const uint32_t f = like->colorFormats[j]) {
            dev.createTexture(64, 64, f, targets[j]);
            pass.color[j].texture = &targets[j];
            pass.count = j + 1;
        }
    raw::Pipeline *drawn = like ? dev.addRecord(record(dev, *like, 0), false, err) : nullptr;
    if (!drawn) {
        std::fprintf(stderr, "no runtime record to draw: %s\n", err.c_str());
        return 1;
    }
    std::atomic<bool> making{true}, drawing{false};
    unsigned made = 0;
    std::thread completion([&] {
        while (!drawing)
            std::this_thread::yield();
        std::string why;
        for (uint32_t n = 1; n <= 200; n++)
            made += dev.addRecord(record(dev, *like, n), false, why) != nullptr;
        making = false;
    });
    unsigned draws = 0, refused = 0;
    while (making) {
        drawing = true;
        raw::CommandBuffer *cb = dev.begin();
        raw::RenderEncoder *e = cb ? cb->render(pass) : nullptr;
        if (!e)
            break;
        e->setPipeline(drawn);
        const char *why = nullptr;
        for (int i = 0; i < 16; i++, draws++)
            refused += !e->draw(3, 0, 3, 1, 0, &why);
        e->end();
        uint32_t seq = 0;
        if (cb->commit(seq))
            dev.wait(seq);
    }
    completion.join();
    std::printf("%u records made beside %u draws (%u refused)\n", made, draws, refused);
    const bool ok = made == 200 && draws && !refused;
    std::printf("%s: racecheck\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
