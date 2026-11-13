// SPDX-License-Identifier: MIT
// The facade's half of private textures on compressed memory (raw_private_comp.h): which descriptors qualify, and the
// move to plain memory before anything names a texture's pages another way.
#import "RawMetalInternal.h"
#import "RawMetalMultisample.h"
#include <algorithm>
#include <mutex>
#include "raw_storage_form.h"
#include "raw_submit_gate.h"
#include "raw_view.h"
#include "raw_xr.h"

thread_local bool tRawEncoding = false;

// The process's shapes a storage form needed plain (rmStorageTargetsPlain): textures of them are made plain
static raw::Mutex gPlainShapesLock;
static raw::PlainShapes gPlainShapes;

static raw::SubmitGate gSubmitGate;
static thread_local bool tSubmitting = false;

bool submitRangeExclusive(RMCommandBuffer *cb, bool followed)
{
    tSubmitting = true;
    const bool submitted = gSubmitGate.submit([&] {
        return submitRange(cb, followed);
    });
    tSubmitting = false;
    return submitted;
}

raw::SubmitGate &rmSubmitGate()
{
    return gSubmitGate;
}

bool rmInSubmission()
{
    return tSubmitting;
}

uint32_t privateCompressedKind(MTLTextureDescriptor *d)
{
    const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
    raw::PrivateTextureFacts facts;
    facts.privateStorage = d.storageMode == MTLStorageModePrivate;
    facts.oneLevel = d.mipmapLevelCount == 1;
    facts.oneSample = d.sampleCount <= 1;
    facts.twoD = d.textureType == MTLTextureType2D;
    facts.renderedAndSampledOnly = d.usage != MTLTextureUsageUnknown &&
                                   !(d.usage & ~NSUInteger(MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead));
    facts.plainColour =
        f && f->rt && !raw::blockCompressed(f) && raw::xrForm(uint32_t(d.pixelFormat)) == raw::XrForm::None;
    facts.bpp = f ? f->bpp : 0;
    facts.compression = (gRM->raw->kernelInfo().flags & TegraGM20BInfoCompression) != 0;
    const uint32_t kind = raw::privateCompressedKind(facts);
    if (!kind)
        return 0;
    std::lock_guard<raw::Mutex> l(gPlainShapesLock);
    return gPlainShapes.holds({uint64_t(d.pixelFormat), uint64_t(d.width), uint64_t(d.height), uint64_t(d.usage)})
               ? 0
               : kind;
}

bool rmTexturePlain(RMTexture *t)
{
    if (!t || !t->_rawOk || !gRM->raw->privateCompressed(t->_raw))
        return true;
    return rmExcludingSubmissions([t]() -> bool {
        if (tRawEncoding) {
            SWITCH_REFUSE_DETAIL("raw", "comp.private", t->_descriptor.pixelFormat,
                                 "a compressed %ux%u texture of format %lu was asked for its pages inside a raw encode",
                                 t->_raw.width, t->_raw.height, (unsigned long)t->_descriptor.pixelFormat);
            return false;
        }
        @synchronized(t) {
            if (!gRM->raw->privateCompressed(t->_raw))
                return true;
            if (!gRM->raw->plainFromCompressed(t->_raw))
                return false;
        }
        gRM->rawSinceM2v = true;
        return true;
    });
}

void rmRangePlain(RMCommandBuffer *cb, size_t from, size_t to)
{
    cb->_plan.forEachResource(from, to, RMResourceKind::Texture, [](const RMRangeResource &use) {
        RMTexture *t = (__bridge RMTexture *)use.object;
        while (t && t->_kind == RMTextureView)
            t = t->_parent;
        rmTexturePlain(t);
        rmStoredZetaForM2v(t);
    });
    for (size_t at = from; at < to; at++)
        if (cb->_plan.operation(at).op == OpRender) {
            MTLRenderPassDescriptor *pass = (__bridge MTLRenderPassDescriptor *)cb->_plan.legacy(at).obj;
            for (id<MTLTexture> zeta : {pass.depthAttachment.texture, pass.stencilAttachment.texture})
                if ([(id)zeta isKindOfClass:[RMTexture class]])
                    rmStoredZetaForM2v((RMTexture *)zeta);
        }
}

// A pipeline binding's raw pipeline (raster.pipeline, .nativePipeline, .nativeVariant as OpPipeline sets them); `pending`:
// a variant still being made, whose storage form is not known yet
static raw::Pipeline *boundPipeline(void *pipeline, void *native, void *variant, bool &pending)
{
    pending = false;
    if (auto *v = static_cast<raw::Variant<raw::Pipeline> *>(variant)) {
        raw::Pipeline *made = v->value();
        pending = !made && v->state() == raw::Variant<raw::Pipeline>::State::Pending;
        return made;
    }
    if (native)
        return static_cast<raw::Pipeline *>(native);
    id state = (__bridge id)pipeline;
    return [state isKindOfClass:[RMPipeline class]] ? ((RMPipeline *)state)->_raw : nullptr;
}

// The storage view's facts of an attachment once it is plain, read without copying the texture another submission may
// be moving
struct PlainView {
    uint32_t bpp, samples;
    bool comptags = false;
};

static bool fitsOncePlain(const raw::Pipeline &pl, uint32_t slot, const raw::Texture &t)
{
    return raw::storageViewMismatch(raw::texelBytes(pl.storageTic[slot]), pl.rec->storageSamples,
                                    PlainView{t.bpp, t.samples}) == raw::StorageFallback::None;
}

static void notePipeline(RMCommandBuffer *cb, const RMPass &pass, const raw::Pipeline *pl, bool pending)
{
    if (pending) {
        cb->_scanStorageTargets = true;
        return;
    }
    if (!pl || !pl->rec)
        return;
    const uint8_t slots = raw::storageSlotsToPlain(pl->storageBits, pl->barrierTwin != nullptr) & pass.compressed;
    for (uint32_t slot = 0; slots >> slot; ++slot) {
        RMTexture *t = (RMTexture *)pass.color[slot].texture;
        if ((slots >> slot & 1) && fitsOncePlain(*pl, slot, t->_raw) &&
            std::find(cb->_plainForStorage.begin(), cb->_plainForStorage.end(), t) == cb->_plainForStorage.end())
            cb->_plainForStorage.push_back(t);
    }
}

void rmNoteStorageTargets(RMCommandBuffer *cb, const RMPass &pass, const Cmd &binding)
{
    bool pending;
    const raw::Pipeline *pl =
        boundPipeline(binding.obj, reinterpret_cast<void *>(uintptr_t(binding.c)), binding.obj2, pending);
    notePipeline(cb, pass, pl, pending);
}

// The draws of [from, to) whose pipeline binding was a variant still being made where it was recorded: ready now, as the
// submission waits for its variants before it encodes
static void noteRangeDraws(RMCommandBuffer *cb, size_t from, size_t to)
{
    const RMPass *pass = nullptr;
    for (size_t at = from; at < to; ++at) {
        const Cmd c = cb->_plan.legacy(at);
        if (c.op == OpRender)
            pass = &cb->_passes[c.x];
        else if (c.op == OpCompute || c.op == OpBlit)
            pass = nullptr;
        if (!pass || !pass->compressed || !isPlanDraw(c.op))
            continue;
        const raw::RasterState &raster = cb->_plan.raster[cb->_plan.renderStates[cb->_plan.draw(at).state].raster];
        bool pending;
        notePipeline(cb, *pass, boundPipeline(raster.pipeline, raster.nativePipeline, raster.nativeVariant, pending),
                     false);
    }
}

void rmStorageTargetsPlain(RMCommandBuffer *cb, size_t from, size_t to)
{
    if (cb->_scanStorageTargets)
        noteRangeDraws(cb, from, to);
    for (RMTexture *t : cb->_plainForStorage) {
        MTLTextureDescriptor *d = t->_descriptor;
        {
            std::lock_guard<raw::Mutex> l(gPlainShapesLock);
            gPlainShapes.add({uint64_t(d.pixelFormat), uint64_t(d.width), uint64_t(d.height), uint64_t(d.usage)});
        }
        rmTexturePlain(t);
    }
    cb->_plainForStorage.clear();
}
