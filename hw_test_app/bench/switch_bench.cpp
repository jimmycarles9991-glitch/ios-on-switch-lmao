#include "hw_test_app/bench/switch_bench.h"
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_compute.h"
#include "hw_test_app/bench/bench_device.h"
#include "hw_test_app/bench/bench_known.h"

namespace {

constexpr const char *kPack = "/System/Library/Extensions/TegraGM20BMetal.raw/pack.bin";
constexpr const char kPlugin[] = "/TegraGM20BMetal.bundle/";
constexpr uint32_t kTriangleStrip = 4, kBgra8 = 80;
constexpr uint64_t kKeyBase = 0xbe7c000000000000ull;
// Placed programs stay in the device's shader heap for its life: a long session starts a fresh device now and then
constexpr uint64_t kRecordsPerDevice = 4096;
constexpr uint32_t kMaxStorage = 16u << 20;
constexpr uint8_t kUnwritten = 0xa5;  // an output buffer's bytes before a kernel runs, so a word it never wrote shows
// QMDV01_07 (clb1c0qmd.h) for a chain: DEPENDENT_QMD_POINTER (word 5); word 6's SEMAPHORE_RELEASE_ENABLE0 (bit 202) and
// DEPENDENT_QMD_SCHEDULE_ENABLE, DEPENDENT_QMD_TYPE GRID and DEPENDENT_QMD_FIELD_COPY (205-207, NAK's QMD 1.7 form,
// qmd.rs set_dependent_qmd_schedule_pre_v02_04); RELEASE0_ADDRESS (words 23-24) with REDUCTION_ENABLE (794) and
// STRUCTURE_SIZE ONE_WORD (799), REDUCTION_OP RED_ADD and FORMAT UNSIGNED_32 both 0, and RELEASE0_PAYLOAD (word 25)
constexpr uint32_t kQmdDependent = 5, kQmdFlags = 6, kQmdRelease0 = 23, kQmdChainMax = 3, kQmdBytes = 256;
constexpr uint32_t kQmdReleaseEnable0 = 1u << 10, kQmdScheduleDependent = 7u << 13;
constexpr uint32_t kRelease0AddOneWord = 1u << 26 | 1u << 31;

// placeCompute caches a placed kernel by its record's address for the device's life, so a record's address must never
// name another kernel: the records outlive every device the bench opens
std::deque<raw::ComputeRecord> &kernelRecords()
{
    static std::deque<raw::ComputeRecord> records;
    return records;
}

// The pack beside the Metal plugin this process loaded, where the plugin itself finds it (TegraGM20BMetalPlugin.mm
// rawPackRefusal); fetchform's path when no plugin is loaded
std::string packPath()
{
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
        const char *name = _dyld_get_image_name(i);
        if (const char *at = name ? strstr(name, kPlugin) : nullptr)
            return std::string(name, at) + "/TegraGM20BMetal.raw/pack.bin";
    }
    return kPack;
}

uint64_t nanoseconds(uint64_t ticks)
{
    static mach_timebase_info_data_t base = [] {
        mach_timebase_info_data_t b;
        mach_timebase_info(&b);
        return b;
    }();
    return ticks * base.numer / base.denom;
}

bool readsPositionW(const uint8_t *sph)
{
    uint32_t word5;
    memcpy(&word5, sph + 20, sizeof word5);
    return word5 & bench::kPositionW;
}

bool valid(const switch_bench_experiment &x, switch_bench_result &r)
{
    const raw::FormatInfo *f = raw::formatInfo(x.format);
    std::string why;
    if (!f || !f->rt)
        why = "not a format the raw path renders";
    else if (!x.width || !x.height || x.width > 2048 || x.height > 2048)
        why = "target size";
    else if (x.rect[0] >= x.rect[2] || x.rect[1] >= x.rect[3] || x.rect[2] > x.width || x.rect[3] > x.height)
        why = "rect outside the target";
    else if (!x.draw_counts || !x.repeats)
        why = "no command buffers";
    const switch_bench_program &p = x.program;
    const bool programmed = x.kind == SWITCH_BENCH_FRAGMENT || x.kind == SWITCH_BENCH_READBACK ||
                            x.kind == SWITCH_BENCH_COMPUTE || x.compare == SWITCH_BENCH_COMPARE_PROGRAM;
    if (why.empty() && programmed &&
        (!p.code || !p.code_bytes || p.code_bytes % 32 || p.code_bytes > 0x20000 || p.data_bytes % 16 ||
         p.data_bytes > 0x10000 || !p.gprs || p.gprs > 255 || (x.kind != SWITCH_BENCH_COMPUTE && !p.sph)))
        why = "program: code in whole bundles at most 128 KiB, data in 16-byte steps at most 64 KiB, 1..255 registers, "
              "an SPH";
    else if (why.empty() && programmed && x.kind != SWITCH_BENCH_COMPUTE && !readsPositionW(p.sph))
        why = "program.sph: a pixel header must read position W (bit 191), or the GPU traps it";
    else if (why.empty() && programmed && p.data_bound && p.data_bound > ((p.data_bytes + 0xff) & ~size_t(0xff)))
        why = "program.dataBound: at most the data padded to 256-byte steps";
    else if (why.empty() && x.output_bytes && (x.output_bytes % 16 || x.output_bytes > 0x10000 || p.data_bytes < 16))
        why = "compute output: 16-byte steps up to 64 KiB, its address in shader data of at least 16 bytes";
    else if (why.empty() && x.storage_bytes &&
             ((x.kind != SWITCH_BENCH_FRAGMENT && x.kind != SWITCH_BENCH_READBACK) || x.storage_bytes % 4 ||
              x.storage_bytes > kMaxStorage || x.storage_va_at % 8 || size_t(x.storage_va_at) + 8 > p.data_bytes))
        why = "storage: 4-byte words, at most 16 MiB, for a draw, its address at an 8-byte step of the data";
    if (why.empty())
        return true;
    bench::fail(r, SWITCH_BENCH_INVALID, why);
    return false;
}

bench::Program program(const switch_bench_experiment &x)
{
    bench::Program p;
    if (x.kind == SWITCH_BENCH_KNOWN) {
        const auto *sph = reinterpret_cast<const uint8_t *>(bench::kKnownSph);
        const auto *code = reinterpret_cast<const uint8_t *>(bench::kKnownCode);
        const auto *data = reinterpret_cast<const uint8_t *>(bench::kKnownData);
        p.sph.assign(sph, sph + sizeof bench::kKnownSph);
        p.code.assign(code, code + sizeof bench::kKnownCode);
        p.data.assign(data, data + sizeof bench::kKnownData);
        p.gprs = bench::kKnownGprs;
        return p;
    }
    const switch_bench_program &s = x.program;
    if (s.sph)
        p.sph.assign(s.sph, s.sph + 80);
    p.code.assign(s.code, s.code + s.code_bytes);
    if (s.data)
        p.data.assign(s.data, s.data + s.data_bytes);
    p.gprs = s.gprs;
    p.dataBound = s.data_bound;
    return p;
}

bench::Methods pairs(const uint32_t *words, uint32_t count)
{
    bench::Methods out;
    for (uint32_t i = 0; words && i < count; i++)
        out.emplace_back(words[2 * i], words[2 * i + 1]);
    return out;
}

// The experiment's draws into colour 0: compressed, its texels through a staging texture, else written and read by the
// CPU. `into` gets the texels
bool drawInto(switch_bench &b, const switch_bench_experiment &x, raw::Pipeline *pipeline,
              const std::vector<uint32_t> &others, bool compressed, uint8_t *into, switch_bench_result &r,
              const raw::Buffer *storage = nullptr)
{
    raw::Device &d = *b.device;
    const uint32_t bpp = raw::formatInfo(x.format)->bpp;
    bench::Target t;
    raw::Texture staging;
    if (compressed && !d.createTexture(x.width, x.height, x.format, staging, true)) {
        bench::fail(r, SWITCH_BENCH_ERROR, "no memory for the staging texture");
        return false;
    }
    bool ran = bench::makeTarget(d, x, others, compressed, t, r);
    t.storage = storage;
    if (ran && compressed) {
        t.staging = &staging;
        r.form = {1, t.color.comptags, t.color.blockLinear, 0, 0, 1, 0, 0};
    }
    ran = ran && bench::timeDraws(b, x, t, pipeline, r);
    if (ran && !(ran = bench::readBack(d, compressed ? staging : t.color, x, bpp, into, r.texel_bytes)))
        bench::fail(r, SWITCH_BENCH_ERROR, "the kernel refused the CPU a read lease on the target");
    if (ran && x.kind != SWITCH_BENCH_NOOP)
        bench::checkAlive(b, x, t, r);
    bench::destroyTarget(d, t);
    if (staging.va)
        d.destroyTexture(staging);
    return ran;
}

// The experiment's storage buffer, zeroed under a CPU write lease (fill's reason), for as long as the experiment runs
struct Storage {
    raw::Device &device;
    raw::Buffer buffer;
    bool made = false;

    Storage(raw::Device &d, uint32_t bytes) : device(d)
    {
        made = bytes && d.createBuffer(bytes, buffer, true);
        raw::HostAccess lease;
        if (made && !(made = d.hostAccess(buffer, true, lease)))
            d.destroyBuffer(buffer);
        if (made)
            memset(buffer.cpu, 0, bytes);
    }

    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;

    ~Storage()
    {
        if (made)
            device.destroyBuffer(buffer);
    }
};

void runRender(switch_bench &b, const switch_bench_experiment &x, switch_bench_result &r)
{
    raw::Device &d = *b.device;
    Storage storage(d, x.storage_bytes);
    if (x.storage_bytes && !storage.made)
        return bench::fail(r, SWITCH_BENCH_ERROR, "no memory for the storage buffer");
    const bool samples = x.kind != SWITCH_BENCH_NOOP && bench::samplesTexture(program(x));
    const raw::PackPipeline *tpl = x.template_key ? bench::fragmentTemplate(d.pack(), x.template_key)
                                   : samples      ? b.textured
                                                  : b.fragment;
    raw::Pipeline *pipeline = nullptr;
    const uint64_t storageVa = storage.made ? storage.buffer.va : 0;
    if (x.kind != SWITCH_BENCH_NOOP && !(pipeline = bench::pixelPipeline(b, x, r, x.single_target != 0, storageVa)))
        return;
    std::vector<uint32_t> others;
    if (tpl && !x.single_target && x.kind != SWITCH_BENCH_NOOP)
        others = bench::otherFormats(d.pack().string(tpl->keyString));
    if (!drawInto(b, x, pipeline, others, x.compressed != 0, r.texels, r, storage.made ? &storage.buffer : nullptr))
        return;
    raw::HostAccess read;
    if (storage.made && r.storage && r.storage_bytes >= x.storage_bytes && d.hostAccess(storage.buffer, false, read))
        memcpy(r.storage, storage.buffer.cpu, x.storage_bytes);
    if (x.compare == SWITCH_BENCH_COMPARE_PLAIN && r.compare_texels) {
        bench::CompareRun plain(x, r);
        drawInto(b, x, pipeline, others, false, r.compare_texels, plain.r);
        plain.report(r);
    }
}

void runCompute(switch_bench &b, const switch_bench_experiment &x, switch_bench_result &r)
{
    if (!b.kernel)
        return bench::fail(r, SWITCH_BENCH_ERROR, "the pack has no kernel the bench can take as a template");
    raw::Device &d = *b.device;
    raw::Buffer output;
    if (x.output_bytes && !d.createBuffer(x.output_bytes, output, true))
        return bench::fail(r, SWITCH_BENCH_ERROR, "no memory for the output buffer");
    bench::Program p = program(x);
    if (output.va)
        memcpy(p.data.data(), &output.va, sizeof output.va);
    raw::ComputeRecord &record = kernelRecords().emplace_back();
    std::string why;
    const raw::ComputeState *state = nullptr;
    if (!bench::kernelRecord(*b.kernel, p, x.threads, record, why))
        bench::fail(r, SWITCH_BENCH_INVALID, why);
    else if (!(state = raw::placeCompute(d, record)))
        bench::fail(r, SWITCH_BENCH_REFUSED, "no room for the kernel in the shader heap");
    else
        r.template_key = b.kernel->keyHash;
    bench::Submission s(b, x, r);
    bool ran = state != nullptr;
    for (uint32_t c = 0; ran && c < x.draw_counts; c++)
        for (uint32_t k = 0; ran && k < x.warmup + x.repeats; k++) {
            uint64_t gpu = 0, wall = 0;
            if (output.cpu)
                memset(output.cpu, kUnwritten, x.output_bytes);
            ran = s.dispatch(state, x.draws[c], gpu, wall);
            if (ran && k >= x.warmup) {
                r.gpu_ns[c * x.repeats + k - x.warmup] = gpu;
                r.wall_ns[c * x.repeats + k - x.warmup] = wall;
            }
        }
    if (ran && output.cpu && r.output && r.output_bytes >= x.output_bytes)
        memcpy(r.output, output.cpu, x.output_bytes);
    uint64_t gpu = 0, wall = 0;
    if (ran && !s.dispatch(state, 0, gpu, wall) && r.status == SWITCH_BENCH_KILLED)
        snprintf(r.why, sizeof r.why, "the channel was dead after the kernel's last fence (a trap the fence missed)");
    if (output.va)
        d.destroyBuffer(output);
}

// Only QMD 0 is launched: the others are the chain's, held in a buffer of the bench's (the address every kernel's
// shader data names), each patched to schedule the next where chain_schedule says
void runChain(switch_bench &b, const switch_bench_experiment &x, switch_bench_result &r)
{
    if (!b.kernel)
        return bench::fail(r, SWITCH_BENCH_ERROR, "the pack has no kernel the bench can take as a template");
    if (!x.chain || x.chain_links > kQmdChainMax || x.output_bytes < 16)
        return bench::fail(r, SWITCH_BENCH_INVALID, "chain: 1..3 QMDs after the experiment's and an output buffer");
    raw::Device &d = *b.device;
    raw::Buffer output, qmds;
    if (!d.createBuffer(x.output_bytes, output, true) || !d.createBuffer(kQmdChainMax * kQmdBytes, qmds, true) ||
        (qmds.va & (kQmdBytes - 1))) {
        if (output.va)
            d.destroyBuffer(output);
        if (qmds.va)
            d.destroyBuffer(qmds);
        return bench::fail(r, SWITCH_BENCH_ERROR, "no memory for the output or the chain's QMDs");
    }
    std::vector<const raw::ComputeState *> states;
    for (uint32_t i = 0; i <= x.chain_links; i++) {
        switch_bench_experiment link = x;
        if (i)
            link.program = x.chain[i - 1];
        bench::Program p = program(link);
        if (p.data.size() < 16) {
            bench::fail(r, SWITCH_BENCH_INVALID,
                        "chain: each kernel's shader data holds the output's and QMDs' addresses");
            break;
        }
        memcpy(p.data.data(), &output.va, 8);
        memcpy(p.data.data() + 8, &qmds.va, 8);
        raw::ComputeRecord &record = kernelRecords().emplace_back();
        std::string why;
        if (!bench::kernelRecord(*b.kernel, p, x.threads, record, why)) {
            bench::fail(r, SWITCH_BENCH_INVALID, why);
            break;
        }
        const raw::ComputeState *state = raw::placeCompute(d, record);
        if (!state) {
            bench::fail(r, SWITCH_BENCH_REFUSED, "no room for the kernel in the shader heap");
            break;
        }
        states.push_back(state);
    }
    const uint64_t counter = output.va + x.output_bytes - 4;
    bench::Submission s(b, x, r);
    uint64_t gpu = 0, wall = 0;
    memset(output.cpu, kUnwritten, x.output_bytes);
    memset(output.cpu + x.output_bytes - 4, 0, 4);
    const bool ran = states.size() == x.chain_links + 1 &&
                     s.record(
                         [&](raw::CommandBuffer &cb) {
                             raw::ComputeEncoder ce(cb, d);
                             bool ok = true;
                             for (uint32_t i = x.chain_links + 1; ok && i-- > 0;) {
                                 ce.setState(states[i]);
                                 ce.benchQmd = [&, i](uint32_t *qmd) {
                                     if (i < x.chain_links && (x.chain_schedule >> i & 1)) {
                                         qmd[kQmdDependent] = uint32_t((qmds.va + uint64_t(i) * kQmdBytes) >> 8);
                                         qmd[kQmdFlags] |= kQmdScheduleDependent;
                                     }
                                     qmd[kQmdFlags] |= kQmdReleaseEnable0;
                                     qmd[kQmdRelease0] = uint32_t(counter);
                                     qmd[kQmdRelease0 + 1] = (uint32_t(counter >> 32) & 0xff) | kRelease0AddOneWord;
                                     qmd[kQmdRelease0 + 2] = 1;
                                     if (i)
                                         memcpy(qmds.cpu + uint64_t(i - 1) * kQmdBytes, qmd, kQmdBytes);
                                     return i == 0;
                                 };
                                 const char *why = nullptr;
                                 ok = ce.dispatch(x.groups, x.threads, &why);
                                 if (!ok)
                                     s.refused(std::string("dispatch refused: ") + (why ? why : "?"));
                             }
                             ce.end();
                             return ok;
                         },
                         gpu, wall);
    if (ran) {
        r.gpu_ns[0] = gpu;
        r.wall_ns[0] = wall;
        if (r.output && r.output_bytes >= x.output_bytes)
            memcpy(r.output, output.cpu, x.output_bytes);
    }
    d.destroyBuffer(output);
    d.destroyBuffer(qmds);
}

}  // namespace

bool switch_bench::open()
{
    auto opened = std::make_unique<raw::Device>();
    pack = packPath();
    if (!opened->open(pack.c_str(), 0, error, raw::Device::Role::Client)) {
        error = pack + ": " + error;
        return false;
    }
    device = std::move(opened);
    fragment = bench::fragmentTemplate(device->pack());
    textured = bench::textureTemplate(device->pack());
    kernel = bench::kernelTemplate(device->pack());
    raw::SamplerDesc nearest, filtered;
    filtered.minFilter = filtered.magFilter = 1;  // MTLSamplerMinMagFilterLinear
    imageMade = device->createTexture(64, 64, kBgra8, image, true);
    samplerMade = imageMade && device->createSampler(nearest, sampler);
    linearMade = samplerMade && device->createSampler(filtered, linear);
    if (!linearMade) {
        error = "no memory for the template's texture and sampler";
        close();
        return false;
    }
    for (uint32_t i = 0; i < 64 * 64; i++)
        reinterpret_cast<uint32_t *>(image.cpu + (i / 64) * image.pitch)[i % 64] = 0xff000000 | i * 0x010307;
    return true;
}

void switch_bench::reopen()
{
    close();
    open();
}

void switch_bench::close()
{
    if (device && linearMade)
        device->destroySampler(linear);
    if (device && samplerMade)
        device->destroySampler(sampler);
    if (device && imageMade)
        device->destroyTexture(image);
    imageMade = samplerMade = linearMade = false;
    device.reset();
    fragment = textured = nullptr;
    kernel = nullptr;
}

namespace bench {

std::vector<uint32_t> otherFormats(const char *key)
{
    std::vector<uint32_t> formats;
    for (unsigned c = 1; c < 8; c++) {
        char tag[8];
        unsigned format = 0;
        snprintf(tag, sizeof tag, " c%u=", c);
        const char *at = strstr(key, tag);
        if (!at || sscanf(at + strlen(tag), "%u", &format) != 1)
            break;
        formats.push_back(format);
    }
    return formats;
}

// The template's inputs as fetchform binds the pack's states (raw/test/fetchform.cpp): the rect's corners as float4
// clip-space positions through identity matrices, the other attributes and uniforms fixed, its textures one picture
void bindTemplate(raw::RenderEncoder *e, const raw::Pipeline &p, const uint32_t rect[4], uint32_t width,
                  uint32_t height, const raw::Texture &image, const raw::Sampler &sampler)
{
    const raw::PackPipeline &r = *p.rec;
    std::vector<uint8_t> v(4 * r.vertexStride);
    const float x0 = float(rect[0]) / width * 2 - 1, x1 = float(rect[2]) / width * 2 - 1;
    const float y0 = 1 - float(rect[1]) / height * 2, y1 = 1 - float(rect[3]) / height * 2;
    for (uint32_t k = 0; k < 4; k++) {
        float *f = reinterpret_cast<float *>(v.data() + k * r.vertexStride);
        f[0] = k & 1 ? x1 : x0;
        f[1] = k & 2 ? y1 : y0;
        f[2] = 0.5f;
        f[3] = 1.f;
        for (uint32_t w = 4; w < r.vertexStride / 4 && w < 8; w++)
            f[w] = float((k >> (w & 1)) & 1);
    }
    e->setBytes(1, r.vertexSlot, v.data(), uint32_t(v.size()));
    float identity[64], uniforms[64];
    for (uint32_t k = 0; k < 64; k++) {
        identity[k] = (k % 16) % 5 == 0 ? 1.f : 0.f;
        uniforms[k] = float(k % 7) / 7.f;
    }
    for (uint32_t k = 0; k < r.entries; k++) {
        const raw::PackEntry &entry = r.entry[k];
        if (entry.kind == raw::EntryUbo)
            e->setBytes(entry.stage, entry.slot, entry.stage == 1 ? identity : uniforms, sizeof uniforms);
        else if (entry.kind == raw::EntryTexture)
            e->setTexture(entry.slot, &image);
        if (entry.kind != raw::EntryUbo && entry.samplerKind == raw::SamplerSlot)
            e->setSampler(entry.sampler, &sampler);
    }
}

// The target and the template's other colours (memoryless); false: no memory, `r` says so
bool makeTarget(raw::Device &d, const switch_bench_experiment &x, const std::vector<uint32_t> &others,
                bool compressible, Target &t, switch_bench_result &r)
{
    if (compressible)
        d.probeCompressNext.store(true, std::memory_order_release);
    if (!d.createTexture(x.width, x.height, x.format, t.color, !compressible, compressible)) {
        bench::fail(r, SWITCH_BENCH_ERROR, "no memory for the target");
        return false;
    }
    if (compressible && !t.color.comptags) {
        d.destroyTexture(t.color);
        bench::fail(r, SWITCH_BENCH_REFUSED, "the target did not get compressible memory (no comptags)");
        return false;
    }
    for (uint32_t format : others) {
        t.others.emplace_back();
        if (!d.createMemoryless(x.width, x.height, format, t.others.back())) {
            t.others.pop_back();
            bench::fail(r, SWITCH_BENCH_ERROR, "no memory for the template's other colours");
            return false;
        }
    }
    return true;
}

void destroyTarget(raw::Device &d, Target &t)
{
    for (raw::Texture &o : t.others)
        d.destroyTexture(o);
    t.others.clear();
    if (t.color.va)
        d.destroyTexture(t.color);
}

void fail(switch_bench_result &r, uint32_t status, const std::string &why)
{
    r.status = status;
    snprintf(r.why, sizeof r.why, "%s", why.c_str());
}

// The GPU's L2 is not coherent with the CPU: a CPU write reaches the next job only when the kernel knows of it. The
// lease's release makes that job drop the L2's lines of the memory first (gpu_exec.c), which otherwise still hold what the
// GPU wrote there before, the previous experiment's texels at the same address.
bool fill(raw::Device &d, raw::Texture &t, const switch_bench_experiment &x, uint32_t bpp)
{
    raw::HostAccess lease;
    if (!d.hostAccess(t, t.cpu, uint64_t(t.pitch) * x.height, true, lease))
        return false;
    for (uint32_t y = 0; y < x.height; y++) {
        uint8_t *row = t.cpu + y * t.pitch;
        if (x.fill)
            memcpy(row, x.fill + size_t(y) * x.width * bpp, size_t(x.width) * bpp);
        else
            memset(row, 0, t.pitch);
    }
    return true;
}

bool readBack(raw::Device &d, const raw::Texture &t, const switch_bench_experiment &x, uint32_t bpp, uint8_t *into,
              size_t bytes)
{
    if (!into || !t.cpu || bytes < size_t(x.width) * x.height * bpp)
        return true;
    raw::HostAccess lease;
    if (!d.hostAccess(t, t.cpu, uint64_t(t.pitch) * x.height, false, lease))
        return false;
    for (uint32_t y = 0; y < x.height; y++)
        memcpy(into + size_t(y) * x.width * bpp, t.cpu + y * t.pitch, size_t(x.width) * bpp);
    return true;
}

raw::Pipeline *pixelPipeline(switch_bench &b, const switch_bench_experiment &x, switch_bench_result &r,
                             bool firstColourOnly, uint64_t storageVa)
{
    raw::Device &d = *b.device;
    bench::Program fs = program(x);
    if (storageVa)
        memcpy(fs.data.data() + x.storage_va_at, &storageVa, sizeof storageVa);
    const raw::PackPipeline *tpl = x.template_key       ? fragmentTemplate(d.pack(), x.template_key)
                                   : samplesTexture(fs) ? b.textured
                                                        : b.fragment;
    if (!tpl && x.template_key) {
        fail(r, SWITCH_BENCH_INVALID, "no plain pack state has the template key");
        return nullptr;
    }
    if (!tpl) {
        fail(r, SWITCH_BENCH_ERROR, "the pack has no state the bench can take as a template");
        return nullptr;
    }
    raw::RuntimeRecord record;
    std::string why;
    const uint64_t key = kKeyBase | ++b.records;
    if (!fragmentRecord(d.pack(), *tpl, fs, x.format, key, pairs(x.methods, x.method_pairs),
                        pairs(x.restore, x.restore_pairs), record, why, firstColourOnly)) {
        fail(r, SWITCH_BENCH_INVALID, why);
        return nullptr;
    }
    // a program bound at dataBound bytes is bound at exactly that many, the bench's measure of the chip's bound
    d.probeExactCbufs.store(fs.dataBound != 0, std::memory_order_relaxed);
    raw::Pipeline *pipeline = d.addRecord(record, false, why);
    d.probeExactCbufs.store(false, std::memory_order_relaxed);
    if (!pipeline)
        fail(r, SWITCH_BENCH_REFUSED, why);
    else
        r.template_key = tpl->keyHash;
    return pipeline;
}

bool timeDraws(switch_bench &b, const switch_bench_experiment &x, Target &t, raw::Pipeline *pipeline,
               switch_bench_result &r)
{
    const uint32_t bpp = raw::formatInfo(x.format)->bpp;
    Submission s(b, x, r);
    raw::Texture *written = t.staging ? t.staging : &t.color;
    for (uint32_t c = 0; c < x.draw_counts; c++)
        for (uint32_t k = 0; k < x.warmup + x.repeats; k++) {
            uint64_t gpu = 0, wall = 0;
            if (x.fill && !fill(*b.device, *written, x, bpp)) {
                fail(r, SWITCH_BENCH_ERROR, "the kernel refused the CPU a write lease on the target");
                return false;
            }
            if (!s.render(t, pipeline, pipeline ? x.draws[c] : 0, gpu, wall))
                return false;
            if (k >= x.warmup) {
                r.gpu_ns[c * x.repeats + k - x.warmup] = gpu;
                r.wall_ns[c * x.repeats + k - x.warmup] = wall;
            }
        }
    return true;
}

CompareRun::CompareRun(const switch_bench_experiment &x, const switch_bench_result &of)
    : r(of), gpu_(size_t(x.draw_counts) * x.repeats), wall_(gpu_.size())
{
    r.status = SWITCH_BENCH_OK;
    r.why[0] = 0;
    r.gpu_ns = gpu_.data();
    r.wall_ns = wall_.data();
    r.form = {};
}

void CompareRun::report(switch_bench_result &into) const
{
    into.compare_status = r.status;
    snprintf(into.compare_why, sizeof into.compare_why, "%s", r.why);
    if (r.status != SWITCH_BENCH_KILLED)
        return;
    into.status = r.status;
    into.lost_error = r.lost_error;
    into.fault_va = r.fault_va;
    snprintf(into.why, sizeof into.why, "compare: %s", r.why);
}

void checkAlive(switch_bench &b, const switch_bench_experiment &x, Target &t, switch_bench_result &r)
{
    uint64_t gpu = 0, wall = 0;
    Target empty = t;
    empty.staging = nullptr;
    if (Submission(b, x, r).render(empty, nullptr, 0, gpu, wall) || r.status != SWITCH_BENCH_KILLED)
        return;
    snprintf(r.why, sizeof r.why, "the channel was dead after the experiment's last fence (a trap the fence missed)");
}

bool Submission::render(Target &t, raw::Pipeline *pipeline, uint32_t draws, uint64_t &gpu, uint64_t &wall)
{
    raw::Device &d = *b_.device;
    raw::CommandBuffer *cb = d.begin();
    if (!cb)
        return error("no channel: " + d.channelError());
    if (t.storage)
        cb->use(*t.storage, true);
    const uint32_t w = x_.width, h = x_.height;
    if (t.staging) {
        raw::BlitEncoder *in = cb->blit();
        if (!in) {
            cb->abandon();
            return refused("the raw path opened no blit encoder");
        }
        in->copyTexels(*t.staging, 0, 0, 0, t.color, 0, 0, 0, w, h);
        in->end();
    }
    raw::PassDesc pass;
    pass.count = 1 + uint32_t(t.others.size());
    pass.color[0].texture = &t.color;
    pass.color[0].load = x_.fill ? raw::ColorAttachment::Load : raw::ColorAttachment::Clear;
    memcpy(pass.color[0].clear, x_.clear, sizeof x_.clear);
    for (size_t i = 0; i < t.others.size(); i++) {
        pass.color[1 + i].texture = &t.others[i];
        pass.color[1 + i].load = raw::ColorAttachment::Clear;
        pass.color[1 + i].store = false;
    }
    raw::RenderEncoder *e = cb->render(pass);
    if (!e) {
        cb->abandon();
        return refused("the raw path cannot render the target");
    }
    e->setViewport(0, 0, w, h, 0, 1);
    e->setScissor(0, 0, w, h);
    if (draws) {
        e->setPipeline(pipeline);
        bindTemplate(e, *pipeline, x_.rect, x_.width, x_.height, t.image ? *t.image : b_.image,
                     t.linear ? b_.linear : b_.sampler);
    }
    for (uint32_t n = 0; n < draws; n++) {
        const char *why = nullptr;
        if (!e->draw(kTriangleStrip, 0, 4, 1, 0, &why)) {
            e->end();
            const raw::Refusal refusal = cb->refusal;
            cb->abandon();
            char line[160];
            snprintf(line, sizeof line, "draw refused: %s (predicate %u: %u %u %u)", why ? why : "?", refusal.id,
                     refusal.a, refusal.b, refusal.c);
            return refused(line);
        }
    }
    e->end();
    if (t.staging) {
        raw::BlitEncoder *out = cb->blit();
        if (!out) {
            cb->abandon();
            return refused("the raw path opened no blit encoder");
        }
        out->copyTexels(t.color, 0, 0, 0, *t.staging, 0, 0, 0, w, h);
        out->end();
    }
    return finish(cb, gpu, wall);
}

bool Submission::record(const std::function<bool(raw::CommandBuffer &)> &body, uint64_t &gpu, uint64_t &wall)
{
    raw::Device &d = *b_.device;
    raw::CommandBuffer *cb = d.begin();
    if (!cb)
        return error("no channel: " + d.channelError());
    if (!body(*cb)) {
        cb->abandon();
        return false;
    }
    return finish(cb, gpu, wall);
}

bool Submission::dispatch(const raw::ComputeState *state, uint32_t dispatches, uint64_t &gpu, uint64_t &wall)
{
    raw::Device &d = *b_.device;
    raw::CommandBuffer *cb = d.begin();
    if (!cb)
        return error("no channel: " + d.channelError());
    {
        raw::ComputeEncoder ce(*cb, d);
        ce.setState(state);
        for (uint32_t n = 0; n < dispatches; n++) {
            const char *why = nullptr;
            if (!ce.dispatch(x_.groups, x_.threads, &why)) {
                ce.end();
                cb->abandon();
                return refused(std::string("dispatch refused: ") + (why ? why : "?"));
            }
        }
        ce.end();
    }
    return finish(cb, gpu, wall);
}

bool Submission::finish(raw::CommandBuffer *cb, uint64_t &gpu, uint64_t &wall)
{
    raw::Device &d = *b_.device;
    const uint32_t stamp = cb->stamp;
    uint32_t seq = 0;
    const uint64_t start = mach_absolute_time();
    const bool committed = cb->commit(seq);
    const bool done = committed && d.wait(seq);
    wall = nanoseconds(mach_absolute_time() - start);
    if (!done) {
        r_.lost_error = d.lostError();
        r_.fault_va = d.lostFaultVa();
        fail(r_, SWITCH_BENCH_KILLED,
             committed ? "the kernel killed the channel" : "the kernel refused the submission");
        return false;
    }
    if (!d.gpuNanoseconds(stamp, gpu))
        gpu = 0;
    d.learnCompleted();
    return true;
}

bool Submission::refused(const std::string &why)
{
    fail(r_, SWITCH_BENCH_REFUSED, why);
    return false;
}

bool Submission::error(const std::string &why)
{
    fail(r_, SWITCH_BENCH_ERROR, why);
    return false;
}

}  // namespace bench

extern "C" {

struct switch_bench *switch_bench_open(char *why, size_t why_size)
{
    auto *b = new switch_bench();
    if (b->open())
        return b;
    snprintf(why, why_size, "%s", b->error.c_str());
    b->close();
    delete b;
    return nullptr;
}

void switch_bench_close(struct switch_bench *bench)
{
    if (!bench)
        return;
    bench->close();
    delete bench;
}

uint32_t switch_bench_texel_bytes(uint32_t format)
{
    const raw::FormatInfo *f = raw::formatInfo(format);
    return f && f->rt ? f->bpp : 0;
}

size_t switch_bench_describe(struct switch_bench *b, char *out, size_t size)
{
    if (!b || !b->device)
        return size_t(snprintf(out, size, "no raw device: %s", b ? b->error.c_str() : "?"));
    const raw::Pack &pack = b->device->pack();
    return size_t(snprintf(out, size,
                           "%s v%u stack %016llx states %u; template %016llx (%s); kernel template %016llx; own GPU VM",
                           b->pack.c_str(), pack.header()->version, (unsigned long long)pack.header()->stack,
                           pack.header()->pipelines, (unsigned long long)(b->fragment ? b->fragment->keyHash : 0),
                           b->fragment ? pack.string(b->fragment->keyString) : "none",
                           (unsigned long long)(b->kernel ? b->kernel->keyHash : 0)));
}

void switch_bench_known_data(uint32_t out[4])
{
    memcpy(out, bench::kKnownData, sizeof bench::kKnownData);
}

size_t switch_bench_known_kernel(uint8_t *out, size_t size)
{
    if (size < sizeof bench::kKnownKernel)
        return 0;
    memcpy(out, bench::kKnownKernel, sizeof bench::kKnownKernel);
    return sizeof bench::kKnownKernel;
}

void switch_bench_run(struct switch_bench *b, const struct switch_bench_experiment *x, struct switch_bench_result *r)
{
    r->status = r->compare_status = SWITCH_BENCH_OK;
    r->why[0] = r->compare_why[0] = 0;
    r->form = {};
    r->launches = 0;
    if (b->records >= kRecordsPerDevice) {
        b->records = 0;
        b->reopen();
    }
    if (!b->device && !b->open())
        return bench::fail(*r, SWITCH_BENCH_ERROR, "no raw device: " + b->error);
    if (!valid(*x, *r))
        return;
    if (x->kind == SWITCH_BENCH_COMPUTE && x->chain_links)
        runChain(*b, *x, *r);
    else if (x->kind == SWITCH_BENCH_COMPUTE)
        runCompute(*b, *x, *r);
    else if (x->kind == SWITCH_BENCH_TWOD)
        bench::runTwod(*b, *x, *r);
    else if (x->kind == SWITCH_BENCH_PACK)
        bench::runPack(*b, *x, *r);
    else
        runRender(*b, *x, *r);
    if (r->status == SWITCH_BENCH_KILLED)
        b->reopen();
}

void switch_bench_fill_surface(struct switch_bench *b, const void *surface, uint32_t format, uint32_t color,
                               struct switch_bench_result *r)
{
    r->status = SWITCH_BENCH_OK;
    r->why[0] = 0;
    if (!b->device && !b->open())
        return bench::fail(*r, SWITCH_BENCH_ERROR, "no raw device: " + b->error);
    bench::fillSurface(*b, surface, format, color, *r);
    if (r->status == SWITCH_BENCH_KILLED)
        b->reopen();
}

}  // extern "C"
