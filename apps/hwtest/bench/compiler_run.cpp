// SPDX-License-Identifier: MIT
#include "apps/hwtest/bench/compiler_draw.h"
#include "apps/hwtest/bench/compiler_pixels.h"
#include "drivers/TegraGM20BMetal/stamps.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <sys/sysctl.h>

namespace bench::compiler {
using namespace gm20b;

namespace {
enum class Readback {
    Initial,
    Warmup,
    Last
};

std::string gpcKhz()
{
    int khz = 0;
    size_t size = sizeof(khz);
    if (sysctlbyname("debug.switch_gpu_gpc_khz", &khz, &size, nullptr, 0) < 0)
        return std::string("unavailable (") + strerror(errno) + ")";
    return size == sizeof(khz) && khz > 0 ? std::to_string(khz) : "invalid clock read";
}

struct Surface {
    Texture tex;
    Buffer initial;
    uint32_t pitch = 0;
    bool depth = false;
};

class Arm {
public:
    Arm(const Input &in, const Seed &seed, bool vendor, unsigned ordinal)
        : in_(in), seed_(seed), vendor_(vendor), ordinal_(ordinal)
    {}

    ~Arm()
    {
        if (b_.device) {
            for (Buffer *buf : buffers_)
                if (buf->va)
                    b_.device->destroyBuffer(*buf);
            for (auto &s : surfaces_)
                if (s.tex.va)
                    b_.device->destroyTexture(s.tex);
            b_.close();
        }
    }

    bool execute(Result &out, std::string &why);

private:
    const Input &in_;
    const Seed &seed_;
    bool vendor_;
    unsigned ordinal_;
    switch_bench b_;
    switch_bench_experiment experiment_{};
    switch_bench_result status_{};
    Pipeline *pipeline_ = nullptr;
    Resources resources_;
    std::deque<Surface> surfaces_;
    std::vector<Buffer *> buffers_;
    std::map<std::pair<uint32_t, uint32_t>, Buffer> blocks_;
    std::map<std::pair<uint32_t, uint32_t>, Surface *> sampled_;
    Buffer readback_;
    uint32_t attachmentCount_ = 0;
    bool buffer(Buffer &buf, uint32_t bytes, bool volatileMemory = false);
    bool setup(std::string &why);
    bool surface(uint32_t w, uint32_t h, uint32_t format, uint32_t samples, bool depth);
    bool pass(CommandBuffer &cb, bool coverage, std::string &why);
    bool records(Result &result, bool coverage);
    bool transfer(CommandBuffer &cb, Surface &s, bool upload);
    bool readImages(Readback when, Result &out, std::string &why);
    bool reset(bool all);
};

void fill(uint8_t *dst, size_t size, uint32_t format, uint32_t seed)
{
    static constexpr uint16_t half[] = {0, 0x8000, 1, 0x83ff, 0x0400, 0x7bff, 0xfbff, 0x7c00, 0xfc00, 0x7e01};
    static constexpr uint32_t fp[] = {0,          0x80000000, 1,          0x807fffff, 0x00800000,
                                      0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7fc01234};
    std::mt19937 rng(seed);
    for (size_t i = 0; i < size; i += 4) {
        uint32_t word = rng();
        const size_t lane = i / 4;
        const bool wide = format == 115 || format == 105 || format == 260;
        const size_t texel = wide ? lane / 2 : lane;
        const size_t edge = (texel + seed + lane % (wide ? 2 : 1)) % std::size(fp);
        if ((texel % 32) < std::size(fp)) {
            if (format == 115)
                word = uint32_t(half[edge]) | (uint32_t(half[(edge + 3) % std::size(half)]) << 16);
            else if (format == 260 && (lane & 1))
                word = (texel & 1) ? 255 : 0;
            else if (format == 105 || format == 55 || format == 260)
                word = fp[edge];
            else
                word = (texel & 1) ? 0xff00ff00u : 0x00ff00ffu;
        }
        if (format == 260 && (lane & 1))
            word &= 255;  // Byte four holds stencil. Bytes five through seven are undefined.
        memcpy(dst + i, &word, 4);
    }
}

bool Arm::buffer(Buffer &buf, uint32_t bytes, bool volatileMemory)
{
    if (!b_.device->createBuffer(bytes, buf, true, true, volatileMemory))
        return false;
    buffers_.push_back(&buf);
    return true;
}

bool Arm::surface(uint32_t w, uint32_t h, uint32_t format, uint32_t samples, bool depth)
{
    auto &d = *b_.device;
    auto &s = surfaces_.emplace_back();
    s.depth = depth;
    const bool made = depth         ? d.createOwnedZeta(w, h, format, samples, false, s.tex)
                      : samples > 1 ? d.createStoredMultisample(w, h, format, samples, s.tex)
                                    : d.createTexture(w, h, format, s.tex, true);
    if (!made || (!depth && !d.sampleRows(s.tex)))
        return false;
    s.pitch = w * samples * s.tex.bpp;
    const uint32_t bytes = s.pitch * h;
    HostAccess lease;
    if (!buffer(s.initial, bytes) || !d.hostAccess(s.initial, true, lease))
        return false;
    fill(s.initial.cpu, bytes, format, seed_.number ^ uint32_t(surfaces_.size() * 2654435761u));
    return lease.finish();
}

bool Arm::setup(std::string &why)
{
    if (!b_.open()) {
        why = b_.error;
        return false;
    }
    auto &d = *b_.device;
    auto record = in_.nak;
    record.split = false;
    record.strideTwin.reset();
    record.variants.clear();
    record.pipeline.variantCount[0] = record.pipeline.variantCount[1] = 0;
    record.pipeline.keyHash ^= vendor_ ? 0x5200000000000000ull : 0x5100000000000000ull;
    if (vendor_) {
        record.programs[0] = in_.program(vendor_, 0);
        record.programs[1] = in_.program(vendor_, 1);
    }
    if (!(pipeline_ = d.addRecord(record, false, why)))
        return false;
    for (const auto &blk : seed_.blocks) {
        Buffer &buf = blocks_[{blk.stage, blk.slot}];
        const uint32_t extent = blk.offset + uint32_t(blk.bytes.size());
        HostAccess lease;
        if (!buffer(buf, extent) || !d.hostAccess(buf, true, lease))
            return false;
        memcpy(buf.cpu + blk.offset, blk.bytes.data(), blk.bytes.size());
        if (!lease.finish())
            return false;
    }
    for (auto format : in_.colors)
        if (!surface(width, height, format, in_.samples, false))
            return false;
    if (in_.depth && !surface(width, height, in_.depth, in_.samples, true))
        return false;
    attachmentCount_ = uint32_t(surfaces_.size());
    for (uint32_t i = 0; i < pipeline_->entries.size(); ++i) {
        auto &e = pipeline_->entries[i];
        const auto key = std::make_pair(uint32_t(e.stage), uint32_t(e.slot));
        if (e.kind == EntryTexture && !sampled_.count(key)) {
            if (!surface(128, 64, ordinal_ & 1 ? 115 : 81, 1, false))
                return false;
            sampled_.emplace(key, &surfaces_.back());
        }
    }
    uint32_t maxBytes = 0;
    for (const Surface &s : surfaces_)
        maxBytes = std::max(maxBytes, s.pitch * s.tex.height);
    if (!buffer(readback_, maxBytes) || !buffer(resources_.reports, 256))
        return false;
    if (!vendor_ || !pipeline_->rec->storageMask)
        return true;
    if (!buffer(resources_.tickets, 0x40400, true))
        return false;
    if (in_.samples == 1)
        return true;
    HostAccess lease;
    if (!buffer(resources_.metadata, kPackHeapElements * 0x50) || !d.hostAccess(resources_.metadata, true, lease))
        return false;
    for (unsigned i = 0; i < kPackHeapElements; ++i) {
        const uint32_t grid[] = {2, 2, 3, 0};
        memcpy(resources_.metadata.cpu + i * 0x50 + 0x30, grid, sizeof grid);
    }
    return lease.finish();
}

bool Arm::transfer(CommandBuffer &cb, Surface &s, bool upload)
{
    Buffer &mem = upload ? s.initial : readback_;
    cb.use(mem, !upload);
    cb.use(s.tex, upload);
    auto *blit = cb.blit();
    if (!blit)
        return false;
    if (s.depth && s.tex.samples == 4) {
        // The depth copy helper takes physical sample coordinates for a zeta surface.
        Texture grid = s.tex;
        grid.width *= 2;
        grid.height *= 2;
        grid.samples = 1;
        blit->copyBufferTexels(mem.va, s.pitch / 2, grid, 0, 0, 0, grid.width, grid.height, upload);
    } else {
        blit->copyBufferTexels(mem.va, s.pitch, s.tex, 0, 0, 0, s.tex.width, s.tex.height, upload);
    }
    blit->end();
    return true;
}

bool Arm::reset(bool all)
{
    uint64_t gpu, wall;
    Submission s(b_, experiment_, status_);
    return s.record(
        [&](CommandBuffer &cb) {
            for (size_t i = 0; i < (all ? surfaces_.size() : attachmentCount_); ++i)
                if (!transfer(cb, surfaces_[i], true))
                    return false;
            return true;
        },
        gpu, wall);
}

bool Arm::pass(CommandBuffer &cb, bool coverage, std::string &why)
{
    PassDesc desc;
    desc.count = uint32_t(in_.colors.size());
    desc.storageMask = pipeline_->rec->storageMask;
    for (size_t i = 0; i < desc.count; ++i) {
        desc.color[i].texture = &surfaces_[i].tex;
        desc.color[i].load = ColorAttachment::Load;
    }
    if (in_.depth)
        desc.depthStencil.texture = &surfaces_[desc.count].tex;
    auto *e = cb.render(desc);
    if (!e)
        return false;
    e->setViewport(0, 0, width, height, 0, 1);
    e->setScissor(0, 0, width, height);
    e->setPipeline(pipeline_);
    for (const auto &blk : seed_.blocks)
        e->setBuffer(blk.stage, blk.slot, &blocks_.at({blk.stage, blk.slot}), blk.offset);
    for (const auto &[key, image] : sampled_)
        e->setTexture(key.first, key.second, &image->tex);
    for (uint32_t i = 0; i < pipeline_->entries.size(); ++i) {
        const auto &entry = pipeline_->entries[i];
        if (entry.kind == EntryTexture && entry.samplerKind == SamplerSlot)
            e->setSampler(entry.stage, entry.sampler, &b_.linear);
    }
    bool ok = (!coverage || e->setVisibility(kVisibilityCounting, &resources_.reports, 64, false)) &&
              draw(*e, *pipeline_, resources_, in_, vendor_, why);
    if (coverage)
        e->finishVisibility();
    e->end();
    return ok;
}

bool Arm::records(Result &result, bool coverage)
{
    HostAccess lease;
    if (!b_.device->hostAccess(resources_.reports, false, lease))
        return false;
    counters::Record stamp[2];
    memcpy(stamp, resources_.reports.cpu, sizeof stamp);
    if (stamp[0].payload != counters::kPayload || stamp[1].payload != counters::kPayload ||
        stamp[1].units <= stamp[0].units)
        return false;
    if (coverage)
        memcpy(&result.visible, resources_.reports.cpu + 64, 8);
    else
        result.times.push_back(ptimerNanoseconds(stamp[1].units - stamp[0].units));
    return lease.finish();
}

bool Arm::readImages(Readback when, Result &out, std::string &why)
{
    auto &d = *b_.device;
    Submission submission(b_, experiment_, status_);
    const size_t count = when == Readback::Initial ? surfaces_.size() : attachmentCount_;
    for (size_t i = 0; i < count; ++i) {
        auto &s = surfaces_[i];
        uint64_t gpu, wall;
        if (!submission.record(
                [&](CommandBuffer &cb) {
                    return transfer(cb, s, false);
                },
                gpu, wall))
            return false;
        const size_t size = size_t(s.tex.height) * s.pitch;
        HostAccess lease;
        if (!d.hostAccess(readback_, false, lease))
            return false;
        std::vector<uint8_t> bytes(readback_.cpu, readback_.cpu + size);
        if (!lease.finish())
            return false;
        if (s.depth)
            for (size_t j = 0; j < size; j += 8)
                memset(bytes.data() + j + 5, 0, 3);
        if (when == Readback::Last) {
            why = difference({i, s.depth, width, height, in_.samples}, out.pixels.at(i), bytes, "draw 0", "draw 100");
            if (!why.empty())
                return false;
            continue;
        }
        HostAccess initial;
        if (!d.hostAccess(s.initial, false, initial))
            return false;
        const bool differs = memcmp(s.initial.cpu, bytes.data(), size) != 0;
        if (!initial.finish())
            return false;
        if (when == Readback::Warmup) {
            if (i < in_.colors.size())
                out.changed |= differs;
            if (i == 0)
                out.varied = !std::equal(bytes.begin() + s.tex.bpp, bytes.end(), bytes.begin());
            out.pixels.push_back(std::move(bytes));
        } else if (differs) {
            why = "initial surface " + std::to_string(i) + " differs from seeded upload";
            return false;
        }
    }
    return true;
}

bool Arm::execute(Result &out, std::string &why)
{
    if (!setup(why)) {
        if (why.empty())
            why = "allocating controlled resources";
        return false;
    }
    Submission s(b_, experiment_, status_);
    for (unsigned i = 0; i <= 100; ++i) {
        uint64_t gpu, wall;
        if (!reset(i == 0) || (!i && !readImages(Readback::Initial, out, why)))
            break;
        HostAccess clear;
        if (!b_.device->hostAccess(resources_.reports, true, clear))
            break;
        memset(resources_.reports.cpu, 0, 80);
        if (!clear.finish())
            break;
        if (i == 1)
            out.clockBefore = gpcKhz();
        if (!s.record(
                [&](CommandBuffer &cb) {
                    return pass(cb, i == 0, why);
                },
                gpu, wall))
            break;
        if (i == 100)
            out.clockAfter = gpcKhz();
        if (!records(out, i == 0)) {
            why = "missing draw timestamps or visibility result";
            return false;
        }
        if (!i && !readImages(Readback::Warmup, out, why))
            break;
        if (i == 100) {
            if (!readImages(Readback::Last, out, why))
                break;
            Target alive;
            alive.color = surfaces_.front().tex;
            experiment_.width = width;
            experiment_.height = height;
            checkAlive(b_, experiment_, alive, status_);
            if (status_.status == SWITCH_BENCH_OK)
                return true;
            break;
        }
    }
    if (why.empty())
        why = *status_.why ? status_.why : "GPU submission or memory lease failed";
    return false;
}
}  // namespace

bool run(const Input &input, const Seed &seed, bool vendor, unsigned ordinal, Result &result, std::string &why)
{
    Arm arm(input, seed, vendor, ordinal);
    return arm.execute(result, why);
}
}  // namespace bench::compiler
