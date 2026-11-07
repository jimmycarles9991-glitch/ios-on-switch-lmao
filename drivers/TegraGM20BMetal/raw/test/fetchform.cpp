// SPDX-License-Identifier: MIT
// Debug builds' GPU context-switch stress and framebuffer-fetch readback, run by hand: the BlueTool stand-in starts it
// once when the Mac posts com.brunostjohn.switch.debug.ctxsw-stress. Never at boot, never in release.
//
// Two raw devices are two GR contexts the host time-slices between. One draws the pack's fetching states that have both
// of metal2vulkan's forms, five ways (kWays): the barrier form with WAIT_FOR_IDLE barriers, with PIXEL_SHADER_BARRIER
// barriers and with each half of those, and the storage form (the pixel interlock's ticket order, no barrier between
// its draws). The other keeps GR busy with large draws, so context switches land between and inside the first one's
// barriers and ticket waits. Each way's images are then compared with the WAIT_FOR_IDLE way's: equal images with the
// interlock and no barriers between its draws mean the ticket order held across draws. One os_log line per way; a way
// that hangs GR logs nothing, the kernel's hang line names it (ENG0's context switch, the hung submission's interlocked
// states).
#include <os/log.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>
#include "../runtime/raw_gpu.h"

namespace {

constexpr const char *kPack = "/System/Library/Extensions/TegraGM20BMetal.raw/pack.bin";
constexpr uint32_t kWidth = 1280, kHeight = 720, kBgra8 = 80;
constexpr uint32_t kStates = 4, kQuads = 32, kHeavyQuads = 256;
constexpr auto kStress = std::chrono::seconds(8);
constexpr uint32_t kClear = 0xff996633;  // BGRA8 of the clear color below (exact in 8 bits)

os_log_t logger()
{
    static os_log_t l = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "fetchform");
    return l;
}

using Barrier = raw::Device::FetchBarrier;

struct Way {
    const char *name;
    raw::PackForm form;
    Barrier barrier;
};

// The first is the reference; the third and fourth split the PS barrier's two methods, so a hang names the one it needs
const Way kWays[] = {
    {"WFI barrier", raw::PackForm::Barrier, Barrier::WaitForIdle},
    {"PS barrier + 0x0f74", raw::PackForm::Barrier, Barrier::PixelShader},
    {"PS barrier alone", raw::PackForm::Barrier, Barrier::PixelShaderOnly},
    {"PS barrier + NO_WFI invalidate", raw::PackForm::Barrier, Barrier::PixelShaderNoWfiInvalidate},
    {"interlock", raw::PackForm::Default, Barrier::PixelShader},
};

bool twoBgra(const raw::Pipeline &p)
{
    return p.colorFormats[0] == kBgra8 && p.colorFormats[1] == kBgra8 && !p.colorFormats[2];
}

// Fetching states with a barrier-form record that fetch color 0, the one read back. Each lookup places the state's
// programs in the device's shader heap, so only the states kept are looked up.
std::vector<uint64_t> fetchStates(raw::Device &d)
{
    std::vector<uint64_t> keys;
    const raw::PackPipeline *pp = d.pack().section<raw::PackPipeline>(raw::PackPipelines);
    for (uint32_t i = 0; i < d.pack().header()->pipelines && keys.size() < kStates; i++) {
        bool fetchesColor0 = false;
        for (uint32_t k = 0; k < pp[i].entries; k++)
            fetchesColor0 |= pp[i].entry[k].kind == raw::EntryFetch && pp[i].entry[k].slot == 0;
        if (!(pp[i].flags & raw::PipelineBarrierForm) || !fetchesColor0)
            continue;
        raw::Pipeline *storage = d.pipeline(pp[i].keyHash);
        raw::Pipeline *barrier = d.pipeline(pp[i].keyHash, raw::PackForm::Barrier);
        if (storage && barrier && twoBgra(*storage) && (storage->storageBits & 1))
            keys.push_back(pp[i].keyHash);
    }
    return keys;
}

raw::Pipeline *heavyState(raw::Device &d)
{
    const raw::PackPipeline *pp = d.pack().section<raw::PackPipeline>(raw::PackPipelines);
    for (uint32_t i = 0; i < d.pack().header()->pipelines; i++)
        if (!(pp[i].flags & raw::PipelineFetches))
            if (raw::Pipeline *p = d.pipeline(pp[i].keyHash); p && twoBgra(*p) && (p->writeBits & 1))
                return p;
    return nullptr;
}

struct Surfaces {
    raw::Texture memoryless, image;
    raw::Sampler sampler;
    std::vector<raw::Texture> targets;  // one per state, write-combined: read back by the CPU as they are
};

bool makeSurfaces(raw::Device &d, uint32_t targets, Surfaces &s)
{
    if (!d.createMemoryless(kWidth, kHeight, kBgra8, s.memoryless) || !d.createTexture(64, 64, kBgra8, s.image, true))
        return false;
    for (uint32_t i = 0; i < 64 * 64; i++)
        reinterpret_cast<uint32_t *>(s.image.cpu + (i / 64) * s.image.pitch)[i % 64] = 0xff000000 | i * 0x010307;
    raw::SamplerDesc sd;
    sd.minFilter = sd.magFilter = 1;
    if (!d.createSampler(sd, s.sampler))
        return false;
    s.targets.resize(targets);
    for (raw::Texture &t : s.targets)
        if (!d.createTexture(kWidth, kHeight, kBgra8, t, true))
            return false;
    return true;
}

// The pack's states' inputs: positions in clip space (a float4 at offset 0, through every vertex buffer's identity
// matrices), a quad per draw shifted along the diagonal so each overlaps the ones before it, and per-draw fragment
// uniforms and vertex colors, so what a fetch reads depends on which draws came before
void bind(raw::RenderEncoder *e, const raw::Pipeline &p, uint32_t draw, const Surfaces &s)
{
    const raw::PackPipeline &r = *p.rec;
    if (r.vertexSlot != 0xff && r.vertexStride >= 16) {
        std::vector<uint8_t> v(4 * r.vertexStride);
        const float x0 = -0.9f + 0.02f * (draw % kQuads), y0 = -0.8f + 0.015f * (draw % kQuads);
        for (uint32_t k = 0; k < 4; k++) {
            float *f = reinterpret_cast<float *>(v.data() + k * r.vertexStride);
            f[0] = x0 + (k & 1 ? 1.2f : 0.f);
            f[1] = y0 + (k & 2 ? 1.2f : 0.f);
            f[3] = 1.f;
            for (uint32_t w = 4; w < r.vertexStride / 4 && w < 8; w++)
                f[w] = float((k >> (w & 1)) & 1);
            if (r.vertexStride >= 40) {
                __fp16 *color = reinterpret_cast<__fp16 *>(v.data() + k * r.vertexStride + 32);
                for (uint32_t c = 0; c < 4; c++)
                    color[c] = __fp16(float((draw * 5 + c * 3 + k) % 11) / 10.f);
            }
        }
        e->setBytes(1, r.vertexSlot, v.data(), uint32_t(v.size()));
    }
    float identity[64], uniforms[64];
    for (uint32_t k = 0; k < 64; k++) {
        identity[k] = (k % 16) % 5 == 0 ? 1.f : 0.f;
        uniforms[k] = float((draw * 37 + k * 11) % 97) / 97.f;
    }
    for (uint32_t k = 0; k < r.entries; k++) {
        const raw::PackEntry &x = r.entry[k];
        if (x.kind == raw::EntryUbo)
            e->setBytes(x.stage, x.slot, x.stage == 1 ? identity : uniforms, sizeof uniforms);
        else if (x.kind == raw::EntryTexture)
            e->setTexture(x.slot, &s.image);
        if (x.kind != raw::EntryUbo && x.samplerKind == raw::SamplerSlot)
            e->setSampler(x.sampler, &s.sampler);
    }
}

// One pass per pipeline (pipelines[i] into targets[i % size]), `quads` draws each, in one command buffer
bool submit(raw::Device &d, const std::vector<raw::Pipeline *> &pipelines, Surfaces &s, uint32_t quads, uint32_t &seq)
{
    raw::CommandBuffer *cb = d.begin();
    for (size_t i = 0; i < pipelines.size(); i++) {
        raw::PassDesc pd;
        pd.count = 2;
        pd.color[0].texture = &s.targets[i % s.targets.size()];
        pd.color[0].load = raw::ColorAttachment::Clear;
        const float clear[4] = {0.6f, 0.4f, 0.2f, 1.f};
        memcpy(pd.color[0].clear, clear, sizeof clear);
        pd.color[1].texture = &s.memoryless;
        pd.color[1].load = raw::ColorAttachment::Clear;
        pd.color[1].store = false;
        raw::RenderEncoder *e = cb->render(pd);
        if (!e) {
            cb->abandon();
            return false;
        }
        e->setViewport(0, 0, kWidth, kHeight, 0, 1);
        e->setScissor(0, 0, kWidth, kHeight);
        e->setPipeline(pipelines[i]);
        for (uint32_t q = 0; q < quads; q++) {
            bind(e, *pipelines[i], q, s);
            const char *why = nullptr;
            if (!e->draw(4, 0, 4, 1, 0, &why)) {
                os_log_error(logger(), "fetchform: a draw refused: %{public}s", why ? why : "?");
                e->end();
                cb->abandon();
                return false;
            }
        }
        e->end();
    }
    return cb->commit(seq);
}

struct Result {
    bool ok = true;
    uint64_t submits = 0, heavySubmits = 0;
    std::vector<std::vector<uint32_t>> images;
};

// `kStress` of fetching submissions on `a` (at most two in flight) beside heavy ones on `b`, then each state's image
// alone
Result run(raw::Device &a, raw::Device &b, const std::vector<raw::Pipeline *> &pipelines, raw::Pipeline *heavy,
           Surfaces &sa, Surfaces &sb)
{
    Result r;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> heavySubmits{0};
    std::thread busy([&] {
        uint32_t last = 0, seq = 0;
        const std::vector<raw::Pipeline *> one{heavy};
        while (!stop.load(std::memory_order_relaxed) && submit(b, one, sb, kHeavyQuads, seq)) {
            if (last && !b.wait(last))
                break;
            last = seq;
            heavySubmits++;
        }
        if (last)
            b.wait(last);
    });
    const auto end = std::chrono::steady_clock::now() + kStress;
    uint32_t last = 0, seq = 0;
    while (r.ok && std::chrono::steady_clock::now() < end) {
        r.ok = submit(a, pipelines, sa, kQuads, seq) && (!last || a.wait(last));
        last = seq;
        r.submits++;
    }
    r.ok = r.ok && a.wait(last);
    stop = true;
    busy.join();
    r.heavySubmits = heavySubmits;
    for (size_t i = 0; r.ok && i < pipelines.size(); i++) {
        r.ok = submit(a, {pipelines[i]}, sa, kQuads, seq) && a.wait(seq);
        const raw::Texture &t = sa.targets[0];
        std::vector<uint32_t> image(kWidth * kHeight);
        for (uint32_t y = 0; r.ok && y < kHeight; y++)
            memcpy(&image[y * kWidth], t.cpu + y * t.pitch, kWidth * 4);
        r.images.push_back(std::move(image));
    }
    return r;
}

void report(const Way &way, const Result &r, const Result &reference, const std::vector<uint64_t> &keys)
{
    char images[160] = "";
    if (!r.ok) {
        os_log_error(logger(),
                     "fetchform: %{public}s: a submission or wait failed after %llu submits (the channel's GPU error "
                     "is in the kernel log)",
                     way.name, (unsigned long long)r.submits);
        return;
    } else if (&r == &reference) {
        uint64_t drawn = 0;
        for (const auto &image : r.images)
            for (uint32_t px : image)
                drawn += px != kClear;
        snprintf(images, sizeof images, "reference images: %llu of %zu px drawn", (unsigned long long)drawn,
                 r.images.size() * kWidth * kHeight);
    } else if (r.images.size() != reference.images.size()) {
        snprintf(images, sizeof images, "no WFI barrier images to compare with");
    } else {
        snprintf(images, sizeof images, "images equal to the WFI barrier way's");
        for (size_t i = 0; i < r.images.size() && i < reference.images.size(); i++) {
            const auto &want = reference.images[i], &got = r.images[i];
            const auto at = std::mismatch(want.begin(), want.end(), got.begin()).first - want.begin();
            if (size_t(at) == want.size())
                continue;
            snprintf(images, sizeof images, "state %016llx first differs at (%u, %u): WFI %08x, here %08x",
                     (unsigned long long)keys[i], uint32_t(at % kWidth), uint32_t(at / kWidth), want[at], got[at]);
            break;
        }
    }
    os_log(logger(),
           "fetchform: %{public}s: %llu submits of %zu states x %u overlapping quads in %lld s beside %llu heavy "
           "submits on a second channel, no hang; %{public}s",
           way.name, (unsigned long long)r.submits, keys.size(), kQuads, (long long)kStress.count(),
           (unsigned long long)r.heavySubmits, images);
}

}  // namespace

int main()
{
    raw::Device a, b;
    std::string err;
    if (!a.open(kPack, 0, err) || !b.open(kPack, 0, err)) {
        os_log_error(logger(), "fetchform: raw device: %{public}s", err.c_str());
        return 1;
    }
    const std::vector<uint64_t> keys = fetchStates(a);
    raw::Pipeline *heavy = heavyState(b);
    Surfaces sa, sb;
    if (keys.empty() || !heavy || !makeSurfaces(a, uint32_t(keys.size()), sa) || !makeSurfaces(b, 1, sb)) {
        os_log_error(logger(), "fetchform: %zu fetching states with both forms in the pack, heavy state %{public}s",
                     keys.size(), heavy ? "found" : "missing");
        return 1;
    }
    Result reference;
    for (const Way &way : kWays) {
        std::vector<raw::Pipeline *> pipelines;
        for (uint64_t key : keys)
            pipelines.push_back(a.pipeline(key, way.form));
        a.fetchBarrier = way.barrier;
        Result r = run(a, b, pipelines, heavy, sa, sb);
        report(way, r, &way == kWays ? r : reference, keys);
        if (&way == kWays)
            reference = std::move(r);
    }
    return 0;
}
