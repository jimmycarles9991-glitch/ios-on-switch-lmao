// The bench's 2D-engine experiments (raw_probe_ops.h, one operation a command buffer so a rejected one names itself in
// the kernel's kill line) and their compare forms: the same operation through the 3D path or the copy engine.
#include <IOSurface/IOSurfaceRef.h>
#include <cstring>
#include "drivers/TegraGM20BMetal/raw/fx/fx_image.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_probe_ops.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_samples.h"
#include "hw_test_app/bench/bench_device.h"

namespace bench {
namespace {

constexpr uint32_t kTriangleStrip = 4;
constexpr uint32_t kA8b8g8r8 = 0xd5;  // NV902D_SET_RENDER_SOLID_PRIM_COLOR_FORMAT_V_A8B8G8R8
constexpr uint64_t kOne = 1ull << 32;
constexpr uint8_t kDstInvert = 0x55;

raw::ProbeSurface surface(const raw::Texture &t, uint32_t fmt2d, uint32_t width, uint32_t height)
{
    raw::ProbeSurface s;
    s.va = t.va;
    s.fmt2d = fmt2d ? fmt2d : raw::probeTwodFormat(t.format);
    s.pitch = t.pitch;
    s.width = width;
    s.height = height;
    s.blockLinear = t.blockLinear;
    s.blockHeight = t.blockHeight;
    return s;
}

raw::ProbeSurface surface(const raw::Texture &t, uint32_t fmt2d = 0)
{
    return surface(t, fmt2d, t.width, t.height);
}

// What the operation reads: the experiment's source, made once, else the bench's picture
class Source {
public:
    Source(switch_bench &b, const switch_bench_experiment &x) : d_(*b.device), texture_(&b.image)
    {
        const switch_bench_source &s = x.source;
        if (!s.data)
            return;
        made_ = d_.createTexture(s.width, s.height, s.format, own_, true);
        if (!made_)
            return;
        const uint32_t bpp = raw::formatInfo(s.format)->bpp;
        for (uint32_t y = 0; y < s.height; y++)
            memcpy(own_.cpu + y * own_.pitch, s.data + size_t(y) * s.width * bpp, size_t(s.width) * bpp);
        texture_ = &own_;
    }

    ~Source()
    {
        if (made_)
            d_.destroyTexture(own_);
    }

    Source(const Source &) = delete;
    Source &operator=(const Source &) = delete;

    const raw::Texture *texture(const switch_bench_experiment &x) const
    {
        return x.source.data && !made_ ? nullptr : texture_;
    }

private:
    raw::Device &d_;
    raw::Texture own_;
    const raw::Texture *texture_;
    bool made_ = false;
};

raw::ProbeOp operation(const switch_bench_experiment &x, const raw::Texture &target, const raw::Texture *read)
{
    const switch_bench_twod &w = x.twod;
    raw::ProbeOp op;
    op.kind = w.op == SWITCH_BENCH_TWOD_FILL ? raw::ProbeOpKind::Fill : raw::ProbeOpKind::Scale;
    op.dx = x.rect[0];
    op.dy = x.rect[1];
    op.dw = x.rect[2] - x.rect[0];
    op.dh = x.rect[3] - x.rect[1];
    op.du = w.du;
    op.dv = w.dv;
    op.sx = w.sx;
    op.sy = w.sy;
    op.bilinear = w.bilinear != 0;
    op.corner = w.corner != 0;
    op.colorFormat = w.color_format;
    memcpy(op.color, w.color, sizeof op.color);
    op.referenceFill = w.reference != 0;
    op.bracketed = w.bracketed != 0;
    op.rop = uint8_t(w.rop);
    if (w.clip[2]) {
        op.clip[0] = w.clip[0];
        op.clip[1] = w.clip[1];
        op.clip[2] = w.clip[2] - w.clip[0];
        op.clip[3] = w.clip[3] - w.clip[1];
    }
    if (read)
        op.src = surface(*read);
    op.dst = surface(target, w.fmt2d);
    return op;
}

// The source's corner copied 1:1 into the multisampled image's sample grid (its samples as one single-sample surface,
// as probeResolve reads them), so each pixel's samples are known texels
raw::ProbeOp gridFill(const raw::Texture &ms, const raw::Texture &read, const raw::fx::MipChain &grid)
{
    raw::ProbeOp op;
    op.kind = raw::ProbeOpKind::Scale;
    op.src = surface(read);
    op.dst = surface(ms, 0, ms.width << grid.sxLog2, ms.height << grid.syLog2);
    op.dw = op.dst.width;
    op.dh = op.dst.height;
    op.du = op.dv = kOne;
    op.corner = true;
    return op;
}

// The sample grid read into `into` by a scale of the experiment's sample mode, filter and origin, in place of the
// driver's resolve, which is this with du = 2 << 32 a sampled axis, sx = du / 2, bilinear, corner (probeResolveOp)
raw::ProbeOp gridScale(const switch_bench_experiment &x, const raw::Texture &ms, const raw::Texture &into,
                       const raw::fx::MipChain &grid)
{
    const switch_bench_twod &w = x.twod;
    raw::ProbeOp op;
    op.kind = raw::ProbeOpKind::Scale;
    op.src = surface(ms, 0, ms.width << grid.sxLog2, ms.height << grid.syLog2);
    op.dst = surface(into);
    op.dw = into.width;
    op.dh = into.height;
    op.du = w.du;
    op.dv = w.dv;
    op.sx = w.sx;
    op.sy = w.sy;
    op.bilinear = w.bilinear != 0;
    op.corner = w.corner != 0;
    op.rop = uint8_t(w.rop);
    return op;
}

// The whole sample grid complemented in place: a fill under ROP3 DSTINVERT, whose colour it never writes, in the grid's
// own 2D format (runTwod holds that to probeSolidFill)
raw::ProbeOp gridInvert(const raw::Texture &ms, const raw::fx::MipChain &grid)
{
    raw::ProbeOp op;
    op.kind = raw::ProbeOpKind::Fill;
    op.dst = surface(ms, 0, ms.width << grid.sxLog2, ms.height << grid.syLog2);
    op.dw = op.dst.width;
    op.dh = op.dst.height;
    op.colorFormat = op.dst.fmt2d;
    op.rop = kDstInvert;
    return op;
}

// The pack's resolve state drawn from `ms` into `into`, as RawMetalEncode.mm's resolve does it
bool resolve3d(raw::CommandBuffer &cb, raw::Device &d, const raw::Texture &ms, const raw::Texture &into, Submission &s)
{
    raw::Pipeline *resolve = d.resolvePipeline(into.format, ms.samples);
    if (!resolve)
        return s.refused("the pack has no resolve state for the format");
    raw::Texture target = into;
    target.format = raw::resolveTargetFormat(into.format);
    raw::PassDesc p;
    p.color[0].texture = &target;
    p.color[0].load = raw::ColorAttachment::DontCare;
    p.color[0].store = true;
    p.count = 1;
    p.width = into.width;
    p.height = into.height;
    raw::RenderEncoder *e = cb.render(p);
    if (!e)
        return s.refused("the raw path cannot render the resolve target");
    const uint32_t samples = ms.samples;
    const char *why = nullptr;
    e->setPipeline(resolve);
    e->setTexture(0, &ms);
    e->setBytes(2, 0, &samples, sizeof samples);
    const bool drawn = e->draw(kTriangleStrip, 0, 4, 1, 0, &why);
    e->end();
    return drawn || s.refused(std::string("resolve draw refused: ") + (why ? why : "?"));
}

// The pack's mip state drawn over the whole of `into` from `read`: each pixel's centre mapped onto the source and sampled
// with its linear sampler (pack/mip.metal), the 3D path's bilinear scale at any ratio
bool sampled3d(raw::CommandBuffer &cb, raw::Device &d, const raw::Texture &read, const raw::Texture &into,
               Submission &s)
{
    raw::Pipeline *mip = d.mipPipeline(into.format);
    if (!mip)
        return s.refused("the pack has no sampled-quad state for the format");
    raw::Texture target = into;
    raw::PassDesc p;
    p.color[0].texture = &target;
    p.color[0].load = raw::ColorAttachment::DontCare;
    p.color[0].store = true;
    p.count = 1;
    p.width = into.width;
    p.height = into.height;
    raw::RenderEncoder *e = cb.render(p);
    if (!e)
        return s.refused("the raw path cannot render the sampled-quad target");
    const float inverseSize[2] = {1.0f / float(into.width), 1.0f / float(into.height)};
    const char *why = nullptr;
    e->setPipeline(mip);
    e->setTexture(0, &read);
    e->setBytes(2, 0, inverseSize, sizeof inverseSize);
    const bool drawn = e->draw(kTriangleStrip, 0, 4, 1, 0, &why);
    e->end();
    return drawn || s.refused(std::string("sampled-quad draw refused: ") + (why ? why : "?"));
}

// A pass that only clears `t`, as the 11 Nov 15:30 timeout's command buffer had before its 2D fill: a clear-only pass
// leaves no draw for the tiled cache drain that follows draws
bool clearOnly(raw::CommandBuffer &cb, const switch_bench_experiment &x, raw::Texture &t, Submission &s)
{
    raw::PassDesc p;
    p.color[0].texture = &t;
    p.color[0].load = raw::ColorAttachment::Clear;
    memcpy(p.color[0].clear, x.clear, sizeof x.clear);
    p.color[0].store = true;
    p.count = 1;
    raw::RenderEncoder *e = cb.render(p);
    if (!e)
        return s.refused("the raw path cannot render the target for the clear");
    e->end();
    return true;
}

// The whole of `from` into `to` by the copy engine, in a command buffer of its own (untimed)
bool copyAll(Submission &s, const raw::Texture &from, raw::Texture &to)
{
    uint64_t gpu = 0, wall = 0;
    return s.record(
        [&](raw::CommandBuffer &cb) {
            raw::BlitEncoder *e = cb.blit();
            if (!e)
                return s.refused("the raw path opened no blit encoder");
            e->copyTexels(from, 0, 0, 0, to, 0, 0, 0, to.width, to.height);
            e->end();
            return true;
        },
        gpu, wall);
}

// The compare form's operation into a second target, read back into r.compare_texels
void compare(switch_bench &b, const switch_bench_experiment &x, const raw::Texture *read, const raw::Texture *ms,
             switch_bench_result &r)
{
    raw::Device &d = *b.device;
    const uint32_t bpp = raw::formatInfo(x.format)->bpp;
    Target t;
    if (!d.createTexture(x.width, x.height, x.format, t.color, true))
        return fail(r, SWITCH_BENCH_ERROR, "no memory for the compare target");
    if (!fill(d, t.color, x, bpp)) {
        d.destroyTexture(t.color);
        return fail(r, SWITCH_BENCH_ERROR, "the kernel refused the CPU a write lease on the compare target");
    }
    Submission s(b, x, r);
    uint64_t gpu = 0, wall = 0;
    bool ran = false;
    if (x.compare == SWITCH_BENCH_COMPARE_PROGRAM) {
        raw::Pipeline *pipeline = pixelPipeline(b, x, r, true);
        t.image = read;
        t.linear = x.twod.op == SWITCH_BENCH_TWOD_SCALE && x.twod.bilinear;
        ran = pipeline && s.render(t, pipeline, 1, gpu, wall);
    } else if (x.compare == SWITCH_BENCH_COMPARE_COPY) {
        ran = s.record(
            [&](raw::CommandBuffer &cb) {
                raw::BlitEncoder *e = cb.blit();
                if (!e)
                    return s.refused("the raw path opened no blit encoder");
                e->copyTexels(*read, 0, 0, 0, t.color, x.rect[0], x.rect[1], 0, x.rect[2] - x.rect[0],
                              x.rect[3] - x.rect[1]);
                e->end();
                return true;
            },
            gpu, wall);
    } else if (x.compare == SWITCH_BENCH_COMPARE_SAMPLED3D) {
        ran = s.record(
            [&](raw::CommandBuffer &cb) {
                return sampled3d(cb, d, *read, t.color, s);
            },
            gpu, wall);
    } else if (x.compare == SWITCH_BENCH_COMPARE_RESOLVE3D && ms) {
        ran = s.record(
            [&](raw::CommandBuffer &cb) {
                return resolve3d(cb, d, *ms, t.color, s);
            },
            gpu, wall);
    }
    if (ran && !readBack(d, t.color, x, bpp, r.compare_texels, r.texel_bytes))
        fail(r, SWITCH_BENCH_ERROR, "the kernel refused the CPU a read lease on the compare target");
    else if (!ran && r.status == SWITCH_BENCH_OK)
        fail(r, SWITCH_BENCH_REFUSED, "no compare form for this operation");
    d.destroyTexture(t.color);
}

}  // namespace

void runTwod(switch_bench &b, const switch_bench_experiment &x, switch_bench_result &r)
{
    raw::Device &d = *b.device;
    const switch_bench_twod &w = x.twod;
    const uint32_t fmt2d = w.fmt2d ? w.fmt2d : raw::probeTwodFormat(x.format);
    if (!fmt2d)
        return fail(r, SWITCH_BENCH_INVALID, "a format the 2D engine names none of");
    if (w.op == SWITCH_BENCH_TWOD_FILL && !raw::probeSolidFill(fmt2d, w.color_format))
        return fail(r, SWITCH_BENCH_INVALID, "a fill's colour format is its destination's own, not sRGB");
    const uint32_t gridFormat = raw::probeTwodFormat(x.format);
    if (w.invert_grid && !raw::probeSolidFill(gridFormat, gridFormat))
        return fail(r, SWITCH_BENCH_INVALID, "a grid inverted by a fill: a format a solid fill writes, not sRGB");
    Source source(b, x);
    const raw::Texture *read = w.op == SWITCH_BENCH_TWOD_FILL ? nullptr : source.texture(x);
    if (w.op != SWITCH_BENCH_TWOD_FILL && !read)
        return fail(r, SWITCH_BENCH_ERROR, "no memory for the source");
    raw::Texture t, ms;
    raw::fx::MipChain grid;
    const bool resolving = w.op == SWITCH_BENCH_TWOD_RESOLVE;
    if (resolving && (!raw::fx::msLayout(x.width, x.height, w.samples, raw::formatInfo(x.format)->bpp, grid) ||
                      !d.createStoredMultisample(x.width, x.height, x.format, w.samples, ms)))
        return fail(r, SWITCH_BENCH_REFUSED, "no multisampled image of the format and samples");
    raw::Texture staging;
    const bool tiled = w.tiled != 0;
    if (!d.createTexture(x.width, x.height, x.format, t, !tiled, tiled) ||
        (tiled && !d.createTexture(x.width, x.height, x.format, staging, true))) {
        for (raw::Texture *made : {&ms, &t, &staging})
            if (made->va)
                d.destroyTexture(*made);
        return fail(r, SWITCH_BENCH_ERROR, "no memory for the target");
    }
    if (tiled && !t.blockLinear) {
        d.destroyTexture(t);
        d.destroyTexture(staging);
        return fail(r, SWITCH_BENCH_REFUSED, "the target's shape does not tile");
    }
    raw::Texture &cpuSide = tiled ? staging : t;
    r.form = {1,
              t.comptags,
              t.blockLinear,
              w.bracketed,
              w.reference,
              resolving ? ms.samples : 1u,
              w.clip[2] != 0,
              w.clear_first,
              w.rop,
              resolving && w.resolve_by_scale && w.invert_grid};
    const raw::ProbeOp op = operation(x, t, read);
    const uint32_t bpp = raw::formatInfo(x.format)->bpp;
    Submission s(b, x, r);
    bool ran = true;
    for (uint32_t c = 0; ran && c < x.draw_counts; c++)
        for (uint32_t k = 0; ran && k < x.warmup + x.repeats; k++) {
            uint64_t gpu = 0, wall = 0;
            if (!(ran = fill(d, cpuSide, x, bpp))) {
                fail(r, SWITCH_BENCH_ERROR, "the kernel refused the CPU a write lease on the target");
                break;
            }
            if (tiled && !(ran = copyAll(s, staging, t)))
                break;
            // a resolve's samples come from a command buffer of their own, so its stamps time the resolve alone
            uint64_t fillGpu = 0, fillWall = 0;
            ran = !resolving || s.record(
                                    [&](raw::CommandBuffer &cb) {
                                        raw::BlitEncoder *e = cb.blit();
                                        if (!e)
                                            return s.refused("the raw path opened no blit encoder");
                                        e->probe(gridFill(ms, *read, grid), read, ms);
                                        e->end();
                                        return true;
                                    },
                                    fillGpu, fillWall);
            ran = ran && s.record(
                             [&](raw::CommandBuffer &cb) {
                                 if (resolving && w.resolve_by_scale) {
                                     raw::BlitEncoder *e = cb.blit();
                                     if (!e)
                                         return s.refused("the raw path opened no blit encoder");
                                     if (w.invert_grid)
                                         e->probe(gridInvert(ms, grid), nullptr, ms);
                                     e->probe(gridScale(x, ms, t, grid), &ms, t);
                                     e->end();
                                     return true;
                                 }
                                 if (resolving)
                                     return cb.probeResolve(ms, t) ||
                                            s.refused("the 2D engine has no resolve for the grid");
                                 if (w.clear_first && !clearOnly(cb, x, t, s))
                                     return false;
                                 raw::BlitEncoder *e = cb.blit();
                                 if (!e)
                                     return s.refused("the raw path opened no blit encoder");
                                 e->probe(op, read, t);
                                 e->end();
                                 return true;
                             },
                             gpu, wall);
            if (ran && k >= x.warmup) {
                r.gpu_ns[c * x.repeats + k - x.warmup] = gpu;
                r.wall_ns[c * x.repeats + k - x.warmup] = wall;
            }
        }
    if (ran && tiled)
        ran = copyAll(s, t, staging);
    if (ran && !(ran = readBack(d, cpuSide, x, bpp, r.texels, r.texel_bytes)))
        fail(r, SWITCH_BENCH_ERROR, "the kernel refused the CPU a read lease on the target");
    if (ran && x.compare != SWITCH_BENCH_COMPARE_NONE && r.compare_texels) {
        CompareRun run(x, r);
        compare(b, x, read, ms.va ? &ms : nullptr, run.r);
        run.report(r);
    }
    if (ran && r.status == SWITCH_BENCH_OK) {
        Target alive;
        alive.color = t;
        checkAlive(b, x, alive, r);
    }
    d.destroyTexture(t);
    if (staging.va)
        d.destroyTexture(staging);
    if (ms.va)
        d.destroyTexture(ms);
}

void fillSurface(switch_bench &b, const void *surface, uint32_t format, uint32_t color, switch_bench_result &r)
{
    raw::Device &d = *b.device;
    const auto s = static_cast<IOSurfaceRef>(const_cast<void *>(surface));
    switch_bench_experiment x = {};
    x.kind = SWITCH_BENCH_TWOD;
    x.format = format;
    x.width = uint32_t(IOSurfaceGetWidth(s));
    x.height = uint32_t(IOSurfaceGetHeight(s));
    x.rect[2] = x.width;
    x.rect[3] = x.height;
    x.twod.op = SWITCH_BENCH_TWOD_FILL;
    x.twod.color_format = kA8b8g8r8;
    x.twod.color[0] = color;
    raw::Texture t;
    if (!raw::probeTwodFormat(format) ||
        !d.createSurfaceTexture(IOSurfaceGetID(s), IOSurfaceGetBaseAddress(s), IOSurfaceGetAllocSize(s), s, 0, x.width,
                                x.height, uint32_t(IOSurfaceGetBytesPerRow(s)), format, t, true))
        return fail(r, SWITCH_BENCH_REFUSED, "the raw path takes no texture over the surface");
    Submission submission(b, x, r);
    uint64_t gpu = 0, wall = 0;
    submission.record(
        [&](raw::CommandBuffer &cb) {
            raw::BlitEncoder *e = cb.blit();
            if (!e)
                return submission.refused("the raw path opened no blit encoder");
            e->probe(operation(x, t, nullptr), nullptr, t);
            e->end();
            return true;
        },
        gpu, wall);
    d.destroyTexture(t);
}

}  // namespace bench
