// gpu.x1.l2_flush_writeback's rounds (switch_bench.h switch_bench_l2_round). The end's host methods ride in the
// bench's draw probe words (gpu.h ProbeWords) ahead of a second pass's one draw, so they run after the pattern pass
// has ended and before the job's own end, which the command buffer makes Internal (no flush) for every arm but
// SWITCH_BENCH_L2_JOB_END.
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include "drivers/TegraGM20BMetal/push.h"
#include "drivers/TegraGM20BMetal/tiled_cache.h"
#include "kits/SwitchGM20B/SwitchGM20BHost.h"
#include "apps/hwtest/bench/bench_device.h"
#include "apps/hwtest/bench/bench_known.h"

namespace {

constexpr uint32_t kRgba32Float = 125;  // MTLPixelFormatRGBA32Float
constexpr uint32_t kTexelBytes = 16;
constexpr uint32_t kTriangleStrip = 4;
constexpr uint32_t kScratchSide = 16;
constexpr uint32_t kMaxSide = 256;
// cla06f.h NVA06F_MEM_OP_B, gone from clb06f.h ("removed for gm20x"); deko3d still writes it on GM20B as gpfifo
// method 0x00B (engine_gpfifo.def:37, gpu_base.cpp:104-109), the raw word 0x80000000
constexpr uint32_t kMemOpB = 0x002c;
// OPERATION (31:27) L2_FLUSH_DIRTY (0x10) of clb06f.h's MEM_OP_D and cla06f.h's MEM_OP_B alike
constexpr uint32_t kL2FlushDirty = 0x10u << 27;

void method(std::vector<uint32_t> &words, uint32_t mthd, uint32_t value)
{
    words.push_back(gm20b::Push::hdr(1, 0, mthd, 1));
    words.push_back(value);
}

// The facade defers a pass's tiled-cache flush to its command buffer's end (pass_end.cpp keepsTiledFlush,
// command_buffer.cpp's tail), after these words: without one here the pattern may still be binned in the ROP when the
// MEM_OP runs, and only the job_end arm would see it in the L2
std::vector<uint32_t> endWords(uint32_t end, bool flushTiles)
{
    std::vector<uint32_t> words;
    if (end != SWITCH_BENCH_L2_BRACKET && end != SWITCH_BENCH_L2_MEM_OP_D && end != SWITCH_BENCH_L2_MEM_OP_B)
        return words;
    if (flushTiles)
        words.push_back(gm20b::Push::hdr(4, gm20b::SUBC_3D, gm20b::TILED_CACHE_FLUSH, 0));
    method(words, NVB06F_WFI, 0);
    method(words, NVB06F_FB_FLUSH, 0);
    if (end == SWITCH_BENCH_L2_MEM_OP_D) {
        words.push_back(gm20b::Push::hdr(1, 0, NVB06F_MEM_OP_C, 2));
        words.push_back(0);
        words.push_back(kL2FlushDirty);
    } else if (end == SWITCH_BENCH_L2_MEM_OP_B) {
        method(words, kMemOpB, kL2FlushDirty);
    }
    method(words, NVB06F_FB_FLUSH, 0);
    return words;
}

void fail(switch_bench_l2_round &r, uint32_t status, const char *why)
{
    r.status = status;
    snprintf(r.why, sizeof r.why, "%s", why);
}

class Round {
public:
    Round(switch_bench &b, switch_bench_l2_round &r) : b_(b), d_(*b.device), r_(r)
    {}

    ~Round()
    {
        disarm();
        bench::destroyTarget(d_, target_);
        if (scratch_.va)
            d_.destroyTexture(scratch_);
    }

    Round(const Round &) = delete;
    Round &operator=(const Round &) = delete;

    void run()
    {
        prepare();
        gm20b::Pipeline *pipeline = nullptr;
        if (!ok() || !(pipeline = bench::pixelPipeline(b_, x_, result_, true)))
            return report();
        if (!bench::makeTarget(d_, x_, {}, false, target_, result_))
            return report();
        r_.wc = target_.color.wc;
        r_.block_linear = target_.color.blockLinear;
        if (!target_.color.wc || target_.color.blockLinear || !target_.color.cpu)
            return fail(r_, SWITCH_BENCH_ERROR, "the target is not write-combined pitch memory with a CPU view");
        if (!d_.createTexture(kScratchSide, kScratchSide, kRgba32Float, scratch_, true))
            return fail(r_, SWITCH_BENCH_ERROR, "no memory for the second pass's target");
        if (!bench::fill(d_, target_.color, x_, kTexelBytes))
            return fail(r_, SWITCH_BENCH_ERROR, "the kernel refused the CPU a write lease on the target");
        const auto body = [&](gm20b::CommandBuffer &cb) {
            return record(cb, *pipeline);
        };
        uint64_t gpu = 0, wall = 0;
        if (!bench::Submission(b_, x_, result_).record(body, gpu, wall))
            return report();
        readBack();
    }

private:
    switch_bench &b_;
    gm20b::Device &d_;
    switch_bench_l2_round &r_;
    switch_bench_experiment x_ = {};
    switch_bench_result result_ = {};
    std::vector<uint8_t> fill_;
    bench::Target target_;
    gm20b::Texture scratch_;
    bool armed_ = false;

    bool ok() const
    {
        return r_.status == SWITCH_BENCH_OK;
    }

    void prepare()
    {
        r_.exact_jobs = d_.exactJobs();
        if (!r_.width || !r_.height || r_.width > kMaxSide || r_.height > kMaxSide ||
            r_.word_count < size_t(r_.width) * r_.height * 4 || !r_.words)
            return fail(r_, SWITCH_BENCH_INVALID, "1..256 texels a side, and room for every word");
        const size_t texels = size_t(r_.width) * r_.height;
        fill_.resize(texels * kTexelBytes);
        for (size_t i = 0; i < texels; i++)
            memcpy(fill_.data() + i * kTexelBytes, r_.sentinel, kTexelBytes);
        x_.kind = SWITCH_BENCH_FRAGMENT;
        x_.format = kRgba32Float;
        x_.width = r_.width;
        x_.height = r_.height;
        x_.rect[2] = r_.width;
        x_.rect[3] = r_.height;
        x_.fill = fill_.data();
        x_.program.sph = reinterpret_cast<const uint8_t *>(bench::kKnownSph);
        x_.program.code = reinterpret_cast<const uint8_t *>(bench::kKnownCode);
        x_.program.code_bytes = sizeof bench::kKnownCode;
        x_.program.data = reinterpret_cast<const uint8_t *>(r_.pattern);
        x_.program.data_bytes = sizeof r_.pattern;
        x_.program.gprs = bench::kKnownGprs;
        x_.draw_counts = 1;
        x_.repeats = 1;
        x_.single_target = 1;
        result_.status = SWITCH_BENCH_OK;
    }

    bool pass(gm20b::CommandBuffer &cb, gm20b::Texture &into, uint32_t width, uint32_t height, bool load,
              gm20b::Pipeline &pipeline, bool carriesEnd)
    {
        gm20b::PassDesc desc;
        desc.count = 1;
        desc.color[0].texture = &into;
        desc.color[0].load = load ? gm20b::ColorAttachment::Load : gm20b::ColorAttachment::Clear;
        gm20b::RenderEncoder *e = cb.render(desc);
        if (!e) {
            fail(r_, SWITCH_BENCH_REFUSED, "the driver cannot render the target");
            return false;
        }
        const uint32_t rect[4] = {0, 0, width, height};
        e->setViewport(0, 0, width, height, 0, 1);
        e->setScissor(0, 0, width, height);
        e->setPipeline(&pipeline);
        bench::bindTemplate(e, pipeline, rect, width, height, b_.image, b_.sampler);
        if (carriesEnd)
            arm();
        const char *why = nullptr;
        const bool drawn = e->draw(kTriangleStrip, 0, 4, 1, 0, &why);
        if (carriesEnd)
            disarm();
        e->end();
        if (!drawn) {
            char line[160];
            snprintf(line, sizeof line, "draw refused: %s", why ? why : "?");
            fail(r_, SWITCH_BENCH_REFUSED, line);
        }
        return drawn;
    }

    bool record(gm20b::CommandBuffer &cb, gm20b::Pipeline &pipeline)
    {
        if (r_.end != SWITCH_BENCH_L2_JOB_END)
            cb.internalCompletion();
        return pass(cb, target_.color, r_.width, r_.height, true, pipeline, false) &&
               pass(cb, scratch_, kScratchSide, kScratchSide, false, pipeline, true);
    }

    void arm()
    {
        std::vector<uint32_t> words = endWords(r_.end, gm20b::tiledCache(d_.kernelInfo()));
        if (words.empty())
            return;
        auto probe = std::make_shared<gm20b::Device::ProbeWords>();
        probe->before = std::move(words);
        {
            std::lock_guard<gm20b::Mutex> hold(d_.probeLock);
            d_.probeWords = std::move(probe);
            d_.probeEmitted.store(0, std::memory_order_relaxed);
        }
        d_.probeArmed.store(true, std::memory_order_release);
        armed_ = true;
    }

    void disarm()
    {
        if (!armed_)
            return;
        d_.probeArmed.store(false, std::memory_order_release);
        r_.emitted = uint32_t(d_.probeEmitted.exchange(0));
        std::lock_guard<gm20b::Mutex> hold(d_.probeLock);
        d_.probeWords.reset();
        armed_ = false;
    }

    // No lease: a CPU read lease makes the kernel write the GPU's L2 back first, which is the thing measured
    void readBack()
    {
        const size_t row = size_t(r_.width) * kTexelBytes;
        auto *into = reinterpret_cast<uint8_t *>(r_.words);
        for (uint32_t y = 0; y < r_.height; y++)
            memcpy(into + y * row, target_.color.cpu + size_t(y) * target_.color.pitch, row);
    }

    void report()
    {
        if (!ok())
            return;
        r_.status = result_.status;
        snprintf(r_.why, sizeof r_.why, "%s", result_.why);
        r_.lost_error = result_.lost_error;
        r_.fault_va = result_.fault_va;
    }
};

}  // namespace

extern "C" void switch_bench_l2_round_run(struct switch_bench *b, struct switch_bench_l2_round *r)
{
    r->status = SWITCH_BENCH_OK;
    r->why[0] = 0;
    r->exact_jobs = r->wc = r->block_linear = r->emitted = r->lost_error = 0;
    r->fault_va = 0;
    if (!b->device && !b->open())
        return fail(*r, SWITCH_BENCH_ERROR, "no raw device");
    Round(*b, *r).run();
    if (r->status == SWITCH_BENCH_KILLED)
        b->reopen();
}
