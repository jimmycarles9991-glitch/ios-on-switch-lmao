// SPDX-License-Identifier: MIT
// DEVELOPMENT: the keyline, glass and blur-texture probes of draws (keyline_probe.h, glass_probe.h,
// glass_mem.h).
#include "lock.h"
#include "keyline_probe.h"
#include "glass_probe.h"
#include "gpu.h"
#include "stamps.h"
#include "tiled_cache.h"

#include <mach/mach_time.h>
#include <os/log.h>
#include <set>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

#if SWITCH_METAL_DIAGNOSTICS
namespace {
// The keyline probe's generation (each armKeylineProbe starts the next), and its armed window's lines left and end
std::atomic<uint32_t> gKeylineGeneration{1};
std::atomic<int32_t> gKeylineLines{0};
std::atomic<int64_t> gKeylineUntil{0};

int64_t keylineNow()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool keylineArmed()
{
    if (gKeylineLines.load(std::memory_order_relaxed) <= 0)
        return false;
    if (keylineNow() < gKeylineUntil.load(std::memory_order_relaxed))
        return true;
    gKeylineLines.store(0, std::memory_order_relaxed);
    return false;
}

// "t<slot>:<w>x<h>/f<format>/<bl|pitch>[+ml][+import]" for each bound texture of `slots` (bit i: slot i, of the first 16)
void describeTextures(char *out, size_t size, const Texture *const *textures, uint32_t slots)
{
    snprintf(out, size, "-");
    size_t at = 0;
    for (uint32_t slot = 0; slot < 16 && at + 1 < size; slot++) {
        const Texture *t = textures[slot];
        if (!(slots >> slot & 1) || !t)
            continue;
        const int n =
            snprintf(out + at, size - at, "%st%u:%ux%u/f%u/%s%s%s", at ? " " : "", slot, t->width, t->height, t->format,
                     t->blockLinear ? "bl" : "pitch", t->memoryless ? "+ml" : "", t->import ? "+import" : "");
        at += std::min(size - at - 1, size_t(std::max(n, 0)));
    }
}
}  // namespace

void armKeylineProbe()
{
    gKeylineLines.store(256, std::memory_order_relaxed);
    gKeylineUntil.store(keylineNow() + 30000, std::memory_order_relaxed);
    gKeylineGeneration.fetch_add(1, std::memory_order_relaxed);
}

// A Core Animation draw (Pipeline::caName), for each form of its state: a solid fill of its three-colour pass its first four
// draws and first twelve thin ones each probe generation, any other state its first four thin draws while the probe is
// armed. Its line: the form drawn (storage: the state's own, inside the pixel interlock; barrier: its twin, as formFor took
// it; barrier-record: the pack's barrier record standing for an unusable first one; split: its split twin; whole: its one
// program), why its storage form fell back, its colour bits, each colour attachment as the pass bound it (format/bytes a
// texel/samples/layout/size, then the load: d don't care, l load, c clear), the textures its record samples and, for a fill
// that samples none, the ones an earlier draw left at slots 0 and 1, the draw's pixels (scissor within viewport), and its
// quads as Core Animation wrote them (keyline_probe.h): how many, how many thin, the first and the first thin one with
// its colour
void RenderEncoder::probeDraw(const Pipeline &bound, Pipeline &drawn, uint32_t primitive, int32_t first, uint32_t count,
                              bool indexed, const PixelBox &limit)
{
    if (glassProbeArmed())
        probeGlassDraw(bound, drawn, limit);
    if (dev_->glassMemLive_)
        glassMemSample(bound, drawn);
    const uint32_t generation = gKeylineGeneration.load(std::memory_order_relaxed);
    if (drawn.probeGeneration != generation) {
        drawn.probeGeneration = generation;
        drawn.probeLines = drawn.probeThinLines = 0;
    }
    const uint8_t anyLines = bound.fillState ? 4 : 0, thinLines = bound.fillState ? 12 : 4;
    if ((drawn.probeLines >= anyLines && drawn.probeThinLines >= thinLines) || (!bound.fillState && !keylineArmed()))
        return;
    const Device &d = *dev_;
    const PackPipeline &rec = *drawn.rec;
    const PackPipeline *packed = d.pack_.section<PackPipeline>(PackPipelines);
    const bool inPack = &rec >= packed && &rec < packed + d.pack_.bytes(PackPipelines) / sizeof(PackPipeline);
    const Bound &matrix = bufs_[1][2];
    const Bound &stream = bufs_[1][rec.vertexSlot < 31 ? rec.vertexSlot : 0];
    const uint64_t skip = first >= 0 ? uint64_t(first) * rec.vertexStride : UINT64_MAX;
    FillStream s;
    if (stream.cpu && skip < stream.size) {
        s.bytes = stream.cpu + skip;
        s.size = stream.size - skip;
    }
    s.stride = rec.vertexStride;
    s.positionOffset = drawn.positionOffset;
    s.positionComponents = drawn.positionComponents;
    s.colorOffset = inPack ? fillColorOffset(d.pack_.string(rec.keyString)) : -1;
    std::vector<uint32_t> indices;
    if (indexed && drawIndices_)
        for (uint32_t i = 0; i < std::min(drawIndexCount_, kFillScanVertices); i++)
            indices.push_back(drawIndexSize_ == 2 ? reinterpret_cast<const uint16_t *>(drawIndices_)[i]
                                                  : reinterpret_cast<const uint32_t *>(drawIndices_)[i]);
    const uint32_t vertices = indexed ? uint32_t(indices.size()) : count;
    const uint32_t group = std::min(primitive == 4 ? vertices : 6u, vertices);
    const FillScan scan = fillScan(s, indexed ? indices.data() : nullptr, vertices, group,
                                   matrix.cpu && matrix.size >= 64 ? matrix.cpu : nullptr, vp_);
    if (drawn.probeLines < anyLines)
        drawn.probeLines++;
    else if (scan.thin && drawn.probeThinLines < thinLines)
        drawn.probeThinLines++;
    else
        return;
    if (!bound.fillState && gKeylineLines.fetch_sub(1, std::memory_order_relaxed) <= 0)
        return;
    const char *form = drawn.splitOf                     ? "split"
                       : drawn.storageBits               ? "storage"
                       : &drawn == bound.barrierTwin     ? "barrier"
                       : rec.flags & PipelineBarrierForm ? "barrier-record"
                                                         : "whole";
    char attachments[8 * 48] = "-";
    size_t used = 0;
    for (uint32_t j = 0; j < pass_.count && j < 8; j++) {
        const Texture *t = pass_.color[j].texture;
        const size_t room = sizeof attachments - used;
        const int n =
            t ? snprintf(attachments + used, room, "%sc%u:%u/%u/%u/%s%s%s/%ux%u/%c", used ? " " : "", j, t->format,
                         t->bpp, t->samples, t->blockLinear ? "bl" : "pitch", t->comptags ? "+comp" : "",
                         memoryless_[j] ? "+ml" : "", t->width, t->height, "dlc"[pass_.color[j].load])
              : snprintf(attachments + used, room, "%sc%u:-", used ? " " : "", j);
        used += std::min(room - 1, size_t(std::max(n, 0)));
    }
    uint32_t sampledSlots = 0;
    for (uint32_t k = 0; k < drawn.entries.size(); k++)
        if (drawn.entries[k].kind == EntryTexture && drawn.entries[k].stage == 2 && drawn.entries[k].slot < 16)
            sampledSlots |= 1u << drawn.entries[k].slot;
    char sampled[16 * 48], left[2 * 48] = "-", colour[64] = "-";
    describeTextures(sampled, sizeof sampled, textures_[1], sampledSlots);
    if (!sampledSlots)
        describeTextures(left, sizeof left, textures_[1], 3);
    const FillQuad &q = scan.thin ? scan.firstThin : scan.first;
    if (q.colored)
        snprintf(colour, sizeof colour, "%.5f,%.5f,%.5f,%.5f", q.color[0], q.color[1], q.color[2], q.color[3]);
    os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b"),
           "SwitchMetal: keyline draw %{public}s key %016llx fill %d form %{public}s fallback %{public}s own %d fetch "
           "%02x "
           "write %02x read %02x storage %02x storewrite %02x att %{public}s samples %{public}s left %{public}s box "
           "%u,%u-%u,%u draw %u/%u%{public}s quads %u thin %u first %.2f,%.2f-%.2f,%.2f thinquad %u "
           "%.2f,%.2f-%.2f,%.2f colour %{public}s",
           bound.caName, (unsigned long long)rec.keyHash, int(bound.fillState), form,
           kStorageFallbackNames[size_t(storageFallback(bound))],
           fetchesOwnColor(drawn.fetchBits, drawn.writeBits, drawn.storageBits), drawn.fetchBits, drawn.writeBits,
           drawn.readBits, drawn.storageBits, drawn.storageWriteBits, attachments, sampled, left, limit.x0, limit.y0,
           limit.x1, limit.y1, primitive, indexed ? drawIndexCount_ : count, indexed ? " indexed" : "", scan.quads,
           scan.thin, scan.first.box[0], scan.first.box[1], scan.first.box[2], scan.first.box[3], scan.firstThin.first,
           scan.firstThin.box[0], scan.firstThin.box[1], scan.firstThin.box[2], scan.firstThin.box[3], colour);
}

// The glass probe's line for a Core Animation draw (glass_probe.h): the form drawn as probeDraw names it, each
// colour attachment as the pass bound it (format/bytes a texel/samples/layout[+comp][+ml]/size/load), the textures its
// record samples and its uniform buffers
void RenderEncoder::probeGlassDraw(const Pipeline &bound, const Pipeline &drawn, const PixelBox &limit)
{
    const PackPipeline &rec = *drawn.rec;
    const char *form = drawn.splitOf                     ? "split"
                       : drawn.storageBits               ? "storage"
                       : &drawn == bound.barrierTwin     ? "barrier"
                       : rec.flags & PipelineBarrierForm ? "barrier-record"
                                                         : "whole";
    char attachments[8 * 48] = "-", sampled[16 * 56] = "-", uniforms[8 * 32] = "-";
    size_t at = 0;
    for (uint32_t j = 0; j < pass_.count && j < 8 && at + 1 < sizeof attachments; j++)
        if (const Texture *t = pass_.color[j].texture) {
            const int n =
                snprintf(attachments + at, sizeof attachments - at, "%sc%u:%u/%u/%u/%s%s%s/%ux%u/%c", at ? " " : "", j,
                         t->format, t->bpp, t->samples, t->blockLinear ? "bl" : "pitch", t->comptags ? "+comp" : "",
                         memoryless_[j] ? "+ml" : "", t->width, t->height, "dlc"[pass_.color[j].load]);
            at += std::min(sizeof attachments - at - 1, size_t(std::max(n, 0)));
        }
    at = 0;
    for (uint32_t k = 0; k < drawn.entries.size() && at + 1 < sizeof sampled; k++)
        if (const PackEntry &e = drawn.entries[k];
            e.kind == EntryTexture && e.stage == 2 && e.slot < 16 && textures_[1][e.slot]) {
            at += at ? size_t(snprintf(sampled + at, sizeof sampled - at, " ")) : 0;
            const int n = glassProbeTexture(sampled + at, sizeof sampled - at, e.slot, *textures_[1][e.slot]);
            at += std::min(sizeof sampled - at - 1, size_t(std::max(n, 0)));
        }
    at = 0;
    for (uint32_t k = 0; k < drawn.entries.size() && at + 1 < sizeof uniforms; k++)
        if (const PackEntry &e = drawn.entries[k]; e.kind == EntryUbo && !bufferStorage(e)) {
            const Bound &b = bufs_[e.stage][e.slot];
            at += at ? size_t(snprintf(uniforms + at, sizeof uniforms - at, " ")) : 0;
            const int n = glassProbeUniform(uniforms + at, sizeof uniforms - at, e.stage, e.slot, b.cpu, b.size);
            at += std::min(sizeof uniforms - at - 1, size_t(std::max(n, 0)));
        }
    char taps[160] = "";
    if (glassProbeBlurState(bound.caName)) {
        const Bound &u = bufs_[2][1];
        taps[0] = ' ';
        glassProbeTaps(taps + 1, sizeof taps - 1, u.cpu, u.size);
    }
    char line[1000];
    snprintf(line, sizeof line, "%s k%016llx %s att %s tex %s uni %s box %u,%u-%u,%u%s", bound.caName,
             (unsigned long long)rec.keyHash, form, attachments, sampled, uniforms, limit.x0, limit.y0, limit.x1,
             limit.y1, taps);
    if (!glassProbeFirst(cb_, line) || gGlassProbeLines.fetch_sub(1, std::memory_order_relaxed) <= 0)
        return;
    os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b"),
           "SwitchMetal: glass draw g%u cb %06llx %{public}s", gGlassProbeGeneration.load(std::memory_order_relaxed),
           (unsigned long long)(uintptr_t(cb_) & 0xffffff), line);
}

// The blur-texture probe's rows (glass_mem.h) of what a Core Animation draw samples: each texture's level 0 and, for a
// mip chain, its middle level, one row each, read at the command buffer's end. Surfaces are left out (other processes write
// them), and the copy engine reads plain memory only: a compressed binding hands it compressed bytes (comp.h).
void RenderEncoder::glassMemSample(const Pipeline &bound, const Pipeline &drawn)
{
    for (uint32_t k = 0; k < drawn.entries.size(); k++) {
        const PackEntry &e = drawn.entries[k];
        if (e.kind != EntryTexture || e.stage != 2 || e.slot >= 16 || !textures_[1][e.slot])
            continue;
        const Texture &t = *textures_[1][e.slot];
        if (t.import)
            continue;
        const uint32_t levels = t.chain ? t.chain->layout.levels : 1;
        const bool compressed = t.comptags && !t.zeta && copyEngineReadsCompressed(compressedKind(t.bpp, 0, t.samples));
        const char unread = t.comptags && !compressed ? 'c'
                            : t.memoryless            ? 'm'
                            : t.zeta                  ? 'z'
                            : t.converted()           ? 'v'
                            : t.sampled               ? 'p'
                            : t.standIn               ? 's'
                            : t.blockW == 1 && t.blockH == 1 && t.bpp && t.bpp <= 16 && t.samples == 1 &&
                                    t.layers == 1 && t.type == fx::kTexTwoD && t.va
                                ? 0
                                : 'o';
        uint64_t header = 0xcbf29ce484222325ull;
        if (t.tic)
            for (uint32_t b = 0; b < 32; b++)
                header = (header ^ dev_->tic_.cpu[32 * t.tic + b]) * 0x100000001b3ull;
        const uint32_t picks[2] = {0, levels > 1 ? (levels + 1) / 2 : 0};
        for (uint32_t p = 0; p < (picks[1] ? 2u : 1u); p++) {
            Texture level;
            if (picks[p] && !chainLevelImage(t, picks[p], level))
                continue;
            const Texture &image = picks[p] ? level : t;
            GlassMemItem it;
            it.key = t.storageRange().va ? t.storageRange().va : t.va;
            it.image = image.va;
            it.width = image.width;
            it.height = image.height;
            it.format = t.format;
            it.levels = levels;
            it.level = picks[p];
            it.bpp = t.bpp;
            it.texels = std::min(kGlassMemTexels, image.width);
            it.flags =
                uint8_t((t.arena ? kGlassMemHeap : 0) | (t.blockLinear ? 0 : kGlassMemPitch) |
                        (compressed ? kGlassMemCompressed : 0) | (unread || !image.height ? kGlassMemUnsampled : 0));
            it.unread = unread ? unread : image.height ? 0 : 'o';
            const int index = dev_->glassMem->sampled(it, bound.caName, t.tic, header, dev_->commitCount_);
            if (index >= 0) {
                cb_->glassMemImages_[index] = image;
                cb_->glassMemSources_[index] = &t;
            }
        }
    }
}

// A write that stops a row or two short of a texture's edges (keyline_probe.h EdgeMiss), once per shape and at most
// 256 shapes each probe generation: what wrote (a pass's render area or its draws' scissors within their viewports, a
// copy, an upload), the texture, the rectangle written and `detail`
void logEdgeMiss(const char *what, const Texture &t, const PixelBox &r, const char *detail)
{
    const EdgeMiss m = edgeMiss(t.width, t.height, r.x0, r.y0, r.x1, r.y1);
    if (!m.any())
        return;
    static gm20b::Mutex lock;
    static std::set<std::array<uint32_t, 8>> seen;
    static uint32_t seenGeneration = 0;
    {
        std::lock_guard<gm20b::Mutex> l(lock);
        if (const uint32_t generation = gKeylineGeneration.load(std::memory_order_relaxed);
            generation != seenGeneration) {
            seen.clear();
            seenGeneration = generation;
        }
        const std::array<uint32_t, 8> shape = {uint32_t(what[0]), t.width, t.height, t.format, r.x0, r.y0, r.x1, r.y1};
        if (seen.size() >= 256 || !seen.insert(shape).second)
            return;
    }
    os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b"),
           "SwitchMetal: edge miss %{public}s %ux%u f%u %{public}s rect %u,%u-%u,%u short top %u bottom %u left %u "
           "right %u"
           "%{public}s",
           what, t.width, t.height, t.format, t.blockLinear ? "bl" : "pitch", r.x0, r.y0, r.x1, r.y1, m.top, m.bottom,
           m.left, m.right, detail);
}
#endif

#if SWITCH_METAL_DIAGNOSTICS
// The blur-texture probe's reads, after everything else the command buffer does: its draws' writes reach memory (the tiled
// cache's flush, the wait for idle) before the copy engine reads each row into a ring slot, read at a later begin() once
// this submission's fence passed. A full ring (no begin() between eight submissions) drops the record.
void CommandBuffer::glassMemTail()
{
    Device &d = *dev_;
    GlassMemCb record =
        d.glassMem->close(d.commitCount_, mach_absolute_time(), d.stats.memEvents.load(std::memory_order_relaxed));
    glassMemSlot_ = -1;
    for (uint32_t i = 0; i < record.count; i++) {
        GlassMemItem &it = record.items[i];
        if (const Texture *t = glassMemSources_[i]; t && t->arena && !t->view && !t->span.valid())
            it.flags |= kGlassMemAliased | kGlassMemUnsampled;
    }
    const GlassMemTailPlan plan = glassMemTailPlan(d.glassMemLive_, record);
    if (!plan.ring)
        return;
    record.launches = plan.launches;
    if (!d.glassMemRing_.va) {
        if (!d.k_.allocControl(uint64_t(kGlassMemSlots) * kGlassMemSlotBytes, d.glassMemRing_,
                               ControlUse::TransferSlot))
            return;
        memset(d.glassMemRing_.cpu, 0, uint64_t(kGlassMemSlots) * kGlassMemSlotBytes);
    }
    int slot = -1;
    for (uint32_t s = 0; s < kGlassMemSlots && slot < 0; s++)
        if (!d.glassMemBusy_[s])
            slot = int(s);
    if (slot < 0)
        return;
    const uint32_t words = 24 + record.count * 256;
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(d.alloc(d.push_, words * 4, 256, va));
    Push p{start, start + words};
    tiledCacheDrain(p, d.k_.info());
    p.m(WAIT_FOR_IDLE, 0);
    const uint64_t base = d.glassMemRing_.va + uint64_t(slot) * kGlassMemSlotBytes;
    const uint64_t cookie = base + uint64_t(kGlassMemItems) * kGlassMemItemBytes, begun = cookie + kGlassMemStartAt;
    p.inc(HOST_SEMAPHORE_A, {uint32_t(begun >> 32) & 0xff, uint32_t(begun), glassMemCookie(record.serial),
                             HOST_SEMAPHORE_RELEASE_16_WFI});
    memset(d.glassMemRing_.cpu + uint64_t(slot) * kGlassMemSlotBytes, kGlassMemPoison,
           uint64_t(record.count) * kGlassMemItemBytes);
    for (uint32_t i = 0; i < record.count; i++) {
        const GlassMemItem &it = record.items[i];
        if (it.flags & kGlassMemUnsampled)
            continue;
        const Texture &image = glassMemImages_[i];
        const uint64_t row = base + uint64_t(i) * kGlassMemItemBytes;
        copySides(p, sideOf(image, it.inX, it.inY, 0), {row, it.texels * it.bpp}, it.texels, 1, it.bpp, d.stats);
        if (it.outside)
            copySides(p, sideOf(image, it.outX, it.outY, 0), {row + kGlassMemRowBytes, it.texels * it.bpp}, it.texels,
                      1, it.bpp, d.stats);
        for (uint32_t q = 0; it.quarters && q < 2; q++)
            copySides(p, sideOf(image, it.inX, it.height * (1 + 2 * q) / 4, 0),
                      {row + (2 + q) * kGlassMemRowBytes, it.texels * it.bpp}, it.texels, 1, it.bpp, d.stats);
    }
    p.inc(HOST_SEMAPHORE_A, {uint32_t(cookie >> 32) & 0xff, uint32_t(cookie), glassMemCookie(record.serial),
                             HOST_SEMAPHORE_RELEASE_16_WFI});
    checkRoom(p, "the glass memory probe's reads");
    segment(start, p.at);
    if (d.k_.exactJobs())
        surfaces_.add(d.glassMemRing_.handle, true);
    d.glassMemCbs_[slot] = record;
    glassMemSlot_ = slot;
}

// At begin(), where the probe is armed or was: armed again after a pause it starts with no history (what was written
// meanwhile went unseen); still armed, the command buffer starts its record; disarmed, the rows in flight are read out
// and nothing else runs until it is armed again.
void Device::glassMemFrame()
{
    const uint64_t now = mach_absolute_time();
    bool on = glassMem && !glassMemStopped.load(std::memory_order_relaxed) && glassMemArming.on(now);
    if (!on) {
        glassMemOn.store(false, std::memory_order_relaxed);
        on = glassMem && !glassMemStopped.load(std::memory_order_relaxed) && glassMemArming.on(now);
        if (on)
            glassMemOn.store(true, std::memory_order_relaxed);
    }
    if (on && !glassMemLive_)
        glassMem->reset();
    glassMemLive_ = on;
    glassMemRead(now);
    if (on)
        glassMem->begin();
}

// The probe's records whose rows landed, oldest first (a reused texture's row is judged against the one its last write
// left): a line for each that found something, or for each while HW Test armed it, and a summary every 5 s, each with
// what the probe's own reads cost. A killed submission stops the probe; a record a reopened channel's fence never
// reaches is dropped after 120 command buffers.
void Device::glassMemRead(uint64_t now)
{
    const os_log_t log = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b");
    const bool every = glassMemArming.every(now);
    for (;;) {
        int next = -1;
        for (uint32_t s = 0; s < kGlassMemSlots; s++)
            if (glassMemBusy_[s] && (next < 0 || glassMemCbs_[s].serial < glassMemCbs_[next].serial))
                next = int(s);
        if (next < 0)
            break;
        if (!done(glassMemCbs_[next].seqno)) {
            if (commitCount_ - glassMemCbs_[next].serial <= 120)
                break;
            glassMemBusy_[next] = false;
            glassMemBusyCount_--;
            continue;
        }
        glassMemBusy_[next] = false;
        glassMemBusyCount_--;
        if (lost(glassMemCbs_[next].seqno)) {
            if (!glassMemStopped.exchange(true))
                os_log_error(
                    log,
                    "SwitchMetal: glassmem: command buffer %llu carrying the probe's reads was killed: the probe is "
                    "off for this process",
                    (unsigned long long)glassMemCbs_[next].serial);
            continue;
        }
        uint8_t rows[kGlassMemSlotBytes];
        memcpy(rows, glassMemRing_.cpu + uint64_t(next) * kGlassMemSlotBytes, sizeof rows);
        const uint32_t cookie = glassMemCookie(glassMemCbs_[next].serial);
        StampRelease begun, ended;
        memcpy(&ended, rows + uint64_t(kGlassMemItems) * kGlassMemItemBytes, sizeof ended);
        memcpy(&begun, rows + uint64_t(kGlassMemItems) * kGlassMemItemBytes + kGlassMemStartAt, sizeof begun);
        const uint64_t probeNs = begun.payload == cookie && ended.payload == cookie && ended.ptimer >= begun.ptimer
                                     ? ptimerNanoseconds(ended.ptimer - begun.ptimer)
                                     : 0;
        for (const std::string &line : glassMem->finish(glassMemCbs_[next], rows, every, probeNs))
            os_log(log, "SwitchMetal: glassmem %{public}s", line.c_str());
    }
    if (!glassMemLive_ || every || now - glassMemSummaryAt_ < glassProbeTicks(5000))
        return;
    const GlassMemCounts &c = glassMem->counts;
    const uint64_t cbs = c.cbs.load() - glassMemSummaryCbs_, per = cbs ? cbs : 1;
    if (cbs)
        os_log(log,
               "SwitchMetal: glassmem totals cbs %llu rebuilt %llu reused %llu zero-reused %llu nf %llu chg %llu mv "
               "%llu ev "
               "%llu probe ce %.1f/cb gpu %.0fus/cb",
               (unsigned long long)c.cbs.load(), (unsigned long long)c.rebuilt.load(),
               (unsigned long long)c.reused.load(), (unsigned long long)c.black.load(),
               (unsigned long long)c.nonFinite.load(), (unsigned long long)c.changed.load(),
               (unsigned long long)c.moved.load(), (unsigned long long)stats.memEvents.load(),
               double(c.launches.load() - glassMemSummaryLaunches_) / double(per),
               double(c.probeNs.load() - glassMemSummaryNs_) / 1000.0 / double(per));
    glassMemSummaryAt_ = now;
    glassMemSummaryCbs_ = c.cbs.load();
    glassMemSummaryLaunches_ = c.launches.load();
    glassMemSummaryNs_ = c.probeNs.load();
}

// A GPU-memory event, numbered: the render server's lines are what a reused texture's /e<n> counts
void Device::glassMemEvent(const char *what, uint64_t bytes)
{
    const uint64_t n = ++stats.memEvents;
    if (glassMemOn.load(std::memory_order_relaxed))
        os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b"),
               "SwitchMetal: glassmem event %llu %{public}s %llu KiB", (unsigned long long)n, what,
               (unsigned long long)(bytes >> 10));
}
#endif

}  // namespace gm20b
