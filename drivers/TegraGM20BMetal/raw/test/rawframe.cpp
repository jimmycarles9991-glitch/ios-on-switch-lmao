// SPDX-License-Identifier: MIT
// raw GM20B path (r7-raw, r8-rawca): test/frame.h's frames through the raw C++ API (runtime/raw_gpu.cpp), over the same mock GPU as nvkframe (cap.cpp decodes
// every submission when RAWCAP_LOG is set). Also the raw encode benchmark: CPU time per frame on the encoding thread.
// usage: rawframe <pack.bin> <corpus.log> <frames> [blurPasses draws uniformBytes]
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <IOSurface/IOSurfaceRef.h>
#else
#include <IOSurface/IOSurface.h>
#endif
#include <algorithm>
#include <mach/mach.h>
#include "../runtime/raw_gpu.h"
#include "frame.h"
#if TARGET_OS_IPHONE
static void pushcount_report(const char *)
{}  // the mock GPU's counter exists on the host only

static void mock_iokit_comp_stats(unsigned long out[5])
{
    for (int i = 0; i < 5; i++)
        out[i] = 0;
}
#else
extern "C" void pushcount_report(const char *what);
extern "C" void mock_iokit_comp_stats(unsigned long out[5]);
#endif

static uint64_t threadNs()
{
    thread_basic_info_data_t t{};
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    mach_port_t self = mach_thread_self();
    thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&t, &n);
    mach_port_deallocate(mach_task_self(), self);
    return ((uint64_t)t.user_time.seconds * 1000000 + t.user_time.microseconds +
            (uint64_t)t.system_time.seconds * 1000000 + t.system_time.microseconds) *
           1000;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: rawframe <pack.bin> <corpus.log> <frames> [blurPasses draws uniformBytes]\n");
        return 2;
    }
    const unsigned frames = atoi(argv[3]);
    frame::Shape shape;
    if (argc > 4)
        shape.blurPasses = atoi(argv[4]);
    if (argc > 5)
        shape.draws = atoi(argv[5]);
    if (argc > 6)
        shape.uniformBytes = atoi(argv[6]);
    if (argc > 7)
        shape.vertexBytes = atoi(argv[7]);
    raw::Device dev;
    std::string err;
    if (!dev.open(argv[1], 0, err)) {
        fprintf(stderr, "raw device: %s\n", err.c_str());
        return 1;
    }
    std::vector<raw::Pipeline *> uber, blur;
    for (auto &s : frame::states(argv[2])) {
        if (!s.uber && !s.blur)
            continue;
        if (s.uber && uber.size() >= shape.mainStates)
            continue;
        if (s.blur && blur.size() >= 4 && !getenv("FRAME_ALL_STATES"))
            continue;
        raw::Pipeline *p = dev.pipeline(raw::Pack::fnv(s.key.data(), s.key.size()));
        if (!p) {
            fprintf(stderr, "NOT IN PACK %s\n", s.key.c_str());
            return 1;
        }
        (s.uber ? uber : blur).push_back(p);
        printf("STATE %s %s\n", s.uber ? "uber" : "blur", s.key.c_str());
    }
    raw::Texture tex[frame::TEX_COUNT];
    for (int t = 0; t < frame::TEX_COUNT; t++) {
        frame::TexDesc td = frame::texture(t);
        bool ok;
        if (td.iosurface) {
            CFMutableDictionaryRef p =
                CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            int w = td.w, h = td.h, bpe = 4, row = td.w * 4, fmt = 'BGRA';
            CFNumberRef nw = CFNumberCreate(nullptr, kCFNumberIntType, &w),
                        nh = CFNumberCreate(nullptr, kCFNumberIntType, &h),
                        nb = CFNumberCreate(nullptr, kCFNumberIntType, &bpe),
                        nr = CFNumberCreate(nullptr, kCFNumberIntType, &row),
                        nf = CFNumberCreate(nullptr, kCFNumberIntType, &fmt);
            CFDictionarySetValue(p, kIOSurfaceWidth, nw);
            CFDictionarySetValue(p, kIOSurfaceHeight, nh);
            CFDictionarySetValue(p, kIOSurfaceBytesPerElement, nb);
            CFDictionarySetValue(p, kIOSurfaceBytesPerRow, nr);
            CFDictionarySetValue(p, kIOSurfacePixelFormat, nf);
            IOSurfaceRef s = IOSurfaceCreate(p);
            ok =
                dev.createSurfaceTexture(IOSurfaceGetID(s), IOSurfaceGetBaseAddress(s), IOSurfaceGetAllocSize(s), s, 0,
                                         td.w, td.h, uint32_t(IOSurfaceGetBytesPerRow(s)), 80, tex[t], td.renderTarget);
        } else if (td.memoryless) {
            ok = dev.createMemoryless(td.w, td.h, td.format, tex[t]);
        } else {  // private in nvkframe: block-linear, as the facade makes a private texture (RawMetal.mm rmNewTexture)
            ok = dev.createTexture(td.w, td.h, td.format, tex[t], false, true);
        }
        if (!ok) {
            fprintf(stderr, "texture %d failed\n", t);
            return 1;
        }
    }
    std::vector<raw::DepthStencilState> dss;
    for (unsigned i = 0; i < frame::kDsSpecs; i++) {
        const frame::DsSpec spec = frame::dsSpec(i);
        raw::DepthStencilDesc desc;
        desc.depthCompare = spec.depthCompare;
        desc.depthWrite = spec.depthWrite;
        raw::StencilFace *faces[2] = {&desc.front, &desc.back};
        for (int k = 0; k < 2; k++) {
            *faces[k] = {spec.face[k][0], spec.face[k][1],  spec.face[k][2],
                         spec.face[k][3], spec.readMask[k], spec.writeMask[k]};
        }
        dss.emplace_back(desc);
        dev.placeSegments(dss.back());
    }
    raw::Sampler smp[2];
    for (int s = 0; s < 2; s++) {
        frame::SamplerSpec sp = frame::sampler(s);
        raw::SamplerDesc d;
        d.minFilter = d.magFilter = uint8_t(sp.minmag);
        d.mipFilter = uint8_t(sp.mip);
        d.sAddress = d.tAddress = d.rAddress = uint8_t(sp.address);
        dev.createSampler(d, smp[s]);
    }
    std::vector<double> cpu;
    std::vector<uint8_t> bytes;
    // BENCH_HARNESS: the frame description and its data only (the cost both drivers share), no encoding
    const bool harness = getenv("BENCH_HARNESS") != nullptr;
    for (unsigned f = 0; f < frames && harness; f++) {
        const uint64_t c0 = threadNs();
        unsigned drawIndex = 0;
        for (const frame::Pass &pass : frame::passes(f, shape, blur.size(), uber.size()))
            for (const frame::Draw &d : pass.draws) {
                for (const frame::Bytes &b : d.bytes)
                    frame::fill(bytes, b, f, drawIndex);
                drawIndex++;
            }
        cpu.push_back((threadNs() - c0) / 1e6);
    }
    raw::Buffer ring, indices, fills, zeros;
    dev.createBuffer(frame::kRingBytes, ring);
    dev.createBuffer(65536, indices);
    dev.createBuffer(frame::kZeroBytes, zeros);
    memset(zeros.cpu, 0, frame::kZeroBytes);
    const bool filling = frame::fillUniforms() && dev.createBuffer(frame::kFillBytes, fills);  // FRAME_FILL (frame.h)
    if (filling)
        memset(fills.cpu, 0, frame::kFillBytes);
    uint64_t ringAt = 0;
    for (unsigned f = 0; f < frames && !harness; f++) {
        const uint64_t c0 = threadNs();
        raw::CommandBuffer *cb = dev.begin();
        if (filling) {
            if (!f)
                cb->clean(fills.cpu, frame::kFillBytes, true);
            raw::BlitEncoder *be = cb->blit();
            be->fillBytes(fills.va + frame::kFillFrom, frame::kFillBytes - frame::kFillFrom, frame::fillValue(f));
            be->end();
        }
        unsigned drawIndex = 0;
        for (const frame::Pass &pass : frame::passes(f, shape, blur.size(), uber.size())) {
            raw::PassDesc pd;
            pd.count = pass.color[2] >= 0 ? 3 : 2;
            for (int c = 0; c < int(pd.count); c++) {
                pd.color[c].texture = &tex[pass.color[c]];
                pd.color[c].load = pass.load[c] == 2   ? raw::ColorAttachment::Clear
                                   : pass.load[c] == 1 ? raw::ColorAttachment::Load
                                                       : raw::ColorAttachment::DontCare;
                pd.color[c].store = !frame::texture(pass.color[c]).memoryless;
                memcpy(pd.color[c].clear, pass.clear, sizeof pass.clear);
            }
            if (frame::rtSize() && !pass.draws.empty() && !pass.draws[0].blur) {
                pd.width = frame::texture(pass.color[0]).w - frame::kRtShortW;
                pd.height = frame::texture(pass.color[0]).h - frame::kRtShortH;
            }
            if (pass.depth >= 0) {
                pd.depthStencil.texture = &tex[pass.depth];
                pd.depthStencil.clearDepth = pd.depthStencil.clearStencil = true;
                pd.depthStencil.depth = float(pass.clearDepth);
                pd.depthStencil.stencil = pass.clearStencil;
            }
            raw::RenderEncoder *e = cb->render(pd);
            if (!e) {
                fprintf(stderr, "pass refused\n");
                return 1;
            }
            for (int stage = 1; stage <= 2; stage++)
                for (int slot = 0; slot < frame::kBufferSlots; slot++)
                    if (!frame::slotFilled(pass, stage, slot))
                        e->setBuffer(stage, slot, &zeros, 0);
            bool bound[3][31] = {};
            for (const frame::Draw &d : pass.draws) {
                e->setPipeline(d.blur ? blur[d.state] : uber[d.state]);
                for (const frame::Bytes &b : d.bytes) {
                    frame::fill(bytes, b, f, drawIndex);
                    if (shape.ring) {
                        if (ringAt + b.length > frame::kRingBytes)
                            ringAt = 0;
                        memcpy(ring.cpu + ringAt, bytes.data(), b.length);
                        if (!bound[b.stage][b.slot]) {
                            e->setBuffer(b.stage, b.slot, &ring, ringAt);
                            bound[b.stage][b.slot] = true;
                        } else
                            e->setBufferOffset(b.stage, b.slot, ringAt);
                        // FRAME_RING_ALIGN: as nvkframe.mm's
                        static const uint64_t align =
                            getenv("FRAME_RING_ALIGN") ? uint64_t(atoi(getenv("FRAME_RING_ALIGN"))) : 256;
                        ringAt = (ringAt + b.length + align - 1) / align * align;
                        continue;
                    }
                    e->setBytes(b.stage, b.slot, bytes.data(), b.length);
                }
                if (const int slot = filling ? frame::fillSlot(d) : -1; slot >= 0) {
                    e->setBuffer(2, uint32_t(slot), &fills, 0);
                    bound[2][slot] = false;
                }
                for (int t = 0; t < 16; t++)
                    if (d.textures[t] >= 0)
                        e->setTexture(t, &tex[d.textures[t]]);
                for (int s = 0; s < 16; s++)
                    if (d.samplers[s] >= 0)
                        e->setSampler(s, &smp[d.samplers[s]]);
                e->setScissor(d.scissor[0], d.scissor[1], d.scissor[2], d.scissor[3]);
                if (d.ds >= 0) {
                    e->setDepthStencil(&dss[d.ds]);
                    e->setStencilReference(d.stencilRef[0], d.stencilRef[1]);
                }
                if (d.raster) {
                    e->setCullMode(d.cull);
                    e->setWinding(d.winding);
                    e->setFillMode(d.fill);
                    e->setDepthBias(d.bias[0], d.bias[1], d.bias[2]);
                }
                const char *why = nullptr;
                if (frame::indexedDraw(d.blur, drawIndex)) {
                    static uint32_t ibAt = 0;
                    const frame::Indexed x = frame::indexed(drawIndex, indices.cpu, ibAt);
                    if (!e->drawIndexed(3, x.count, 2, &indices, x.offset, x.instances, x.baseVertex, x.baseInstance,
                                        &why)) {
                        fprintf(stderr, "draw refused: %s\n", why);
                        return 1;
                    }
                } else if (!e->draw(d.primitive, d.first, d.count, d.instances, d.baseInstance, &why)) {
                    fprintf(stderr, "draw refused: %s\n", why);
                    return 1;
                }
                drawIndex++;
            }
            e->end();
        }
        uint32_t seq = 0;
        if (!cb->commit(seq)) {
            fprintf(stderr, "commit failed\n");
            return 1;
        }
        cpu.push_back((threadNs() - c0) / 1e6);
        dev.wait(seq);
    }
    const size_t warm = cpu.size() > 20 ? 10 : 0;
    std::vector<double> c(cpu.begin() + warm, cpu.end());
    std::sort(c.begin(), c.end());
    double sum = 0;
    for (double x : c)
        sum += x;
    if (!c.empty())
        printf("%s frames=%zu blur=%u draws=%u: encode cpu mean %.3f ms p50 %.3f p90 %.3f | push %.1f KiB/frame upload "
               "%.1f KiB/frame ring waits %llu\n",
               harness ? "harness" : "raw", c.size(), shape.blurPasses, shape.draws, sum / c.size(), c[c.size() / 2],
               c[c.size() * 9 / 10], dev.stats.pushBytes.load() / 1024.0 / frames,
               dev.stats.uploadBytes.load() / 1024.0 / frames, (unsigned long long)dev.stats.ringWaits.load());
    printf("state segments per frame: %.1f binds, %.1f binds of the held segment, %.0f words not copied\n",
           double(dev.stats.segmentBinds.load()) / frames, double(dev.stats.segmentSkips.load()) / frames,
           double(dev.stats.segmentWords.load()) / frames);
    if (dev.stats.compMade.load() || dev.stats.compFailed.load()) {
        unsigned long m[5];
        mock_iokit_comp_stats(m);
        printf(
            "comp: held %llu (%llu KiB) views %llu made %llu failed %llu, zbc clears %llu hit %llu miss | kernel binds "
            "%lu refused %lu alias %lu cpu %lu nolines %lu\n",
            (unsigned long long)dev.stats.compHeld.load(), (unsigned long long)(dev.stats.compHeldBytes.load() >> 10),
            (unsigned long long)dev.stats.compViews.load(), (unsigned long long)dev.stats.compMade.load(),
            (unsigned long long)dev.stats.compFailed.load(), (unsigned long long)dev.stats.zbcHits.load(),
            (unsigned long long)dev.stats.zbcMisses.load(), m[0], m[1], m[2], m[3], m[4]);
    }
    pushcount_report("rawframe");
    return 0;
}
