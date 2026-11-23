// SPDX-License-Identifier: MIT
// See render_encoder.h (RenderEncoder::prepare's inputs: a stage's root table, which set-0 entries moved, the blend and
// argument table words, the region test's geometry, a fetch's attachment views, the raster state and a colour
// matrix form's constants).
#include "lock.h"
#include "nonfinite.h"
#include "clear_box.h"
#include "icb.h"
#include "dynamic_push.h"
#include "gpu.h"
#include "layers.h"
#include "kit.h"
#include "mme.h"
#include "cbuf.h"
#include "residual.h"
#include "mixed_samples.h"
#include "multisample.h"
#include "rotated_pass.h"
#include "fragment_tests.h"
#include "shader_tests.h"
#include "shader_test_push.h"
#include "srgb_table.h"
#include "storage.h"
#include "tiled_cache.h"
#include "view.h"
#include "xr_pass.h"
#include "written_textures.h"

#include <mach/mach_time.h>
#include <os/log.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

// Stage `k`'s (0 vertex, 1 fragment) root table for this draw, in the upload ring: m2v's {null, sink, an entry per buffer slot
// it reaches through the table} over the identity resident set (heap_tables.h). A buffer reached so may be written.
uint64_t RenderEncoder::rootTable(const PackPipeline &r, uint32_t k, bool writes)
{
    Device &d = *dev_;
    const uint32_t slots = r.rootSlots[k], entries = drawRootEntries(slots);
    RootSlot slot[32];
    for (uint32_t n = 0; kFirstSlotRoot + n < entries; n++) {
        const Bound &b = bufs_[k + 1][n];
        slot[n] = {};
        if (!(slots >> n & 1) || !b.va)
            continue;
        if (b.buffer) {
            cb_->use(*b.buffer, true, writes);
            slot[n] = {b.buffer->va, b.buffer->length, b.offset};
        } else {
            cb_->useAddress(b.va, b.size, true);
            slot[n] = {b.va, b.size, 0};
        }
    }
    uint64_t va = 0;
    auto *table = reinterpret_cast<RootEntry *>(d.alloc(d.upload_, entries * sizeof(RootEntry), 256, va));
    writeDrawRoots(table, va, slots, slot, d.heapTables_.sinkVa(), d.heapTables_.identityVa(), d.zero_.va);
    SWITCH_METAL_DIAGNOSTIC(d.stats.uploadBytes += entries * sizeof(RootEntry));
    return va;
}

// A colour matrix form's draw: its iterated blend program and constants from the matrix uniform the draw binds, which
// the GPU's macro reads through a non-prefetched GPFIFO entry over the uniform's bytes (the CPU reads none of them). A
// uniform of the rows alone (a QuartzCore whose matrix has no clamp) is followed by a clamp pair of 0 in an entry of its
// own: the macro's method data runs on across the entries, each but the last incomplete, which the kernel takes only
// before a non-prefetched entry.
bool RenderEncoder::matrixConstants(const Pipeline &pl, const char **why)
{
    static_assert(kMatrixWords == SWITCH_GM20B_MACRO_VCM_ARGUMENTS, "the colour matrix macro's arguments");
    const Bound &b = bufs_[2][pl.freeForm.matrixSlot];
    const uint32_t words = pl.freeForm.matrixWords;
    if (!b.va || uint64_t(pl.freeForm.matrixOffset) + 4 * words > b.size || ((b.va + pl.freeForm.matrixOffset) & 3)) {
        *why = "a colour matrix form's draw binds no uniform buffer that holds the matrix";
        cb_->refusal = {RefMatrixUniform, pl.freeForm.matrixSlot, pl.freeForm.matrixOffset, b.size};
        return false;
    }
    if (b.buffer && b.buffer->hostVisible && !b.buffer->wc && b.cpu)
        cb_->clean(b.cpu + pl.freeForm.matrixOffset, 4 * words);
    mme::matrixHeader(p_);
    dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_->push_.bo.cpu);
    const uint32_t clampWords = kMatrixWords - words;
    uint64_t clampVa = 0;
    if (clampWords) {
        auto *clamp = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, 4 * clampWords, 4, clampVa));
        std::fill(clamp, clamp + clampWords, 0u);
    }
    matrixCall(bodies_, bodyStart_, p_.at, b.va + pl.freeForm.matrixOffset, words, clampVa, clampWords);
    bodyStart_ = nullptr;
    reserve();
    return true;
}

// What the region test reads of a draw: Core Animation's transform (Pipeline::caTransform) over a triangle list or strip
// of one instance, with its matrix, positions and indices where the CPU maps them; anything else is its scissor alone
bool RenderEncoder::entryMoved(const PackEntry &e) const
{
    const bool buffer = e.kind == EntryUbo && (moved_.buffers[e.stage] >> e.slot & 1);
    const bool texture = (e.kind == EntryTexture || e.kind == EntryTexture3d) && e.slot < kStageTextures &&
                         hasSlot(moved_.textures[handleStage(e)], e.slot);
    const bool sampler =
        e.kind != EntryUbo && e.samplerKind == SamplerSlot && (moved_.samplers[handleStage(e)] >> e.sampler & 1);
    const bool spill = e.kind == EntrySpillTable && (moved_.buffers[e.stage] || (e.stage == 2 && spillDirty_));
    return buffer || texture || sampler || spill;
}

// The entries of a set of kPackEntries or fewer whose bindings moved since the last draw, bit k entry k
uint32_t RenderEncoder::movedEntries(const SetEntries &entries) const
{
    if (moved_.all)
        return ~0u;
    uint32_t moved = 0;
    for (uint32_t i = 0; i < entries.size(); i++)
        moved |= uint32_t(entryMoved(entries[i])) << i;
    return moved;
}

bool RenderEncoder::anyEntryMoved(const SetEntries &entries) const
{
    if (moved_.all)
        return true;
    for (uint32_t i = 0; i < entries.size(); i++)
        if (entryMoved(entries[i]))
            return true;
    return false;
}

// `pl`'s argument tables (spill_table.h) whose entries moved: the record's words with the spilled bind offsets'
// residuals and a fragment table's dynamic words, uploaded where they differ from the encoder's last upload, named in set 0
void RenderEncoder::fillSpills(const Pipeline &pl, uint32_t moved, bool all, SetResiduals &prepared, uint32_t *set)
{
    const PackPipeline &r = *pl.rec;
    Device &d = *dev_;
    for (uint32_t k = 0; k < 2; k++) {
        const uint32_t entry = spillEntryOf(r, k), count = r.spillWords[k];
        if (!entry || !(all || (entry <= 32 && moved >> (entry - 1) & 1)))
            continue;
        uint32_t words[kSpillWordsMax];
        composeSpill(pl.spill[k], count, prepared.spilled[k], words);
        if (k == 1) {
            patchBlendPush(r, blendColor_, words, kRootPushWords, count);
            patchShaderTestPush(r.shaderTests, ds_, stencilRef_, depthRange_, false, words, kRootPushWords, count);
        }
        const uint64_t va = spillUploads_[k].resolve(words, count, [&](const uint32_t *from, uint32_t n) {
            SWITCH_METAL_ENCODE_COUNT(DescriptorUploads);
            uint64_t address = 0;
            uint8_t *upload = d.alloc(d.upload_, spillRange(n), 256, address);
            memcpy(upload, from, n * sizeof(uint32_t));
            return address;
        });
        spillDescriptor(va, count, set + pl.entries[entry - 1].offset / 4);
    }
}

DrawGeometry RenderEncoder::geometry(const Pipeline &pl, uint32_t primitive, int32_t first, uint32_t count,
                                     const PixelBox &limit)
{
    DrawGeometry g;
    g.limit = limit;
    g.viewport = {vp_[0], vp_[1], vp_[2], vp_[3]};
    g.first = first;
    g.count = count;
    g.indices = drawIndices_;
    g.indexSize = drawIndices_ ? drawIndexSize_ : 0;
    g.strip = primitive == 4;
    const Bound &m = bufs_[1][2];
    const Bound &v = pl.rec->vertexSlot < 31 ? bufs_[1][pl.rec->vertexSlot] : bufs_[1][0];
    // only what the recording copied (setBytes) is read: a transform, vertices or indices in a buffer leave it unbounded
    g.bounded = !indirectGeometry_ && pl.caTransform && pl.positionComponents && (primitive == 3 || primitive == 4) &&
                !m.buffer && m.cpu && m.size >= 64 && !v.buffer && v.cpu && pl.rec->vertexSlot < 31;
    g.bounded &= !g.indices || drawIndexCpuSafe_;
    if (g.bounded) {
        g.matrix = m.cpu;
        g.positions = {v.cpu, pl.rec->vertexStride, pl.positionOffset, pl.positionComponents};
        g.positionBytes = v.size;
    }
    return g;
}

uint32_t RenderEncoder::passViewTic(const Texture &t, uint32_t word0)
{
    AttachmentView v;
    v.word0 = word0;
    for (uint8_t i = 0; i < passViewCount_; ++i)
        if (passViews_[i].ownTic == t.tic && passViews_[i].view == v)
            return passViews_[i].tic;
    const uint32_t tic = dev_->viewTic(t, word0, ViewAccess::Storage);
    if (tic && passViewCount_ < passViews_.size())
        passViews_[passViewCount_++] = {t.tic, v, tic};
    return tic;
}

uint32_t RenderEncoder::passViewTic(const Texture &owner, const AttachmentView &v, const char **why)
{
    for (uint8_t i = 0; i < passViewCount_; ++i)
        if (passViews_[i].ownTic == owner.tic && passViews_[i].view == v)
            return passViews_[i].tic;
    const uint32_t tic = dev_->attachmentViewTic(owner, v, why);
    if (tic && passViewCount_ < passViews_.size())
        passViews_[passViewCount_++] = {owner.tic, v, tic};
    return tic;
}

uint32_t RenderEncoder::fetchTic(const ColorAttachment &a, uint32_t storage, bool layeredRecord, const char **why)
{
    const Texture &t = *a.texture;
    const bool layered = pass_.layers > 1;
    if (t.tic && (storage ? !layeredRecord : !layered)) {
        if (!storage)
            return t.tic;
        const uint32_t tic = passViewTic(t, storage);
        if (!tic)
            *why = "no texture header left for a framebuffer storage view";
        return tic;
    }
    const FormatInfo *f = formatInfo(t.format);
    if (!f) {
        *why = "framebuffer fetch from a target the driver cannot sample";
        return 0;
    }
    AttachmentView v;
    v.word0 = storage ? storage : f->tic0;
    v.srgb = !storage && f->srgb;
    v.arrayed = storage ? layeredRecord : layered;
    v.layers = v.arrayed ? pass_.layers : 1;
    v.part = a.image != nullptr;
    v.level = uint8_t(a.level);
    v.slice = a.slice;
    return passViewTic(v.part ? *a.image : t, v, why);
}

// The rasterizer state of a draw of `primitive` (MTLPrimitiveType, or icb::kPatchPrimitive): the encoder's, with
// Metal's depth bias for its topology: on, of Metal's factors, for triangles and the triangles of patches (an M4 Max
// biases a tessellated quad patch as a triangle); off and zero for points and lines, and zero where both factors are
RenderEncoder::Raster RenderEncoder::rasterFor(uint32_t primitive) const
{
    Raster r = raster_;
    r.bias = icb::triangles(primitive);
    if (r.bias && (depthBias_.constant != 0 || depthBias_.slope != 0)) {
        // Metal's unit of an n-bit UNORM depth is one code, 1 / (2^n - 1) (an M4 Max's Depth16Unorm, macOS 27.2.1)
        const uint32_t bits = depthBias_.unorm24 ? 0 : pass_.biasDepthBits;
        r.units = bits || depthBias_.unorm24 ? Raster::Units::Unorm24 : Raster::Units::Format;
        const double scale = bits ? 16777216.0 / double((1u << bits) - 1) : 1.0;
        r.biasConstant = fbits(float(double(depthBias_.constant) * scale));
        r.biasSlope = fbits(depthBias_.slope);
        r.biasClamp = fbits(depthBias_.clamp);
    }
    return r;
}

}  // namespace gm20b
