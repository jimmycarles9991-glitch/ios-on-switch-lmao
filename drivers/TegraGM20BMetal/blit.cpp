// SPDX-License-Identifier: MIT
// See blit_encoder.h. The copies follow NVK's nouveau_copy_rect and nvk_cmd_copy_memory_ce (MIT).
#include "lock.h"
#include "fx/fx_image.h"
#include "gpu.h"
#include "kit.h"
#include "zeta_cpu.h"
#include "fx/fx_mip_route.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "blit_remap.h"

namespace gm20b {

LoopStream BlitEncoder::stream()
{
    return cb_->loopStream(p_, start_);
}

BlitEncoder *CommandBuffer::blit()
{
    BlitEncoder &e = blit_;
    e = BlitEncoder();
    heldSegment_.lose();
    e.cb_ = this;
    e.dev_ = dev_;
    uint64_t va;
    e.start_ = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, 1 << 14, 256, va));
    e.p_ = Push{e.start_, e.start_ + (1 << 12)};
    loops_.openSegment(e.stream());
    // what the render work before wrote is complete before the copy engine reads it (NVK: a WFI on the 3D subchannel; the
    // switch to the copy subchannel then waits for the 3D engine anyway)
    const BarrierForms forms = barrierForms();
    [[maybe_unused]] const Opened opened =
        blitOpening(e.p_, switchFormFor(forms.switchForm, lastEngine_, BarrierEngine::Copy), entryFormFor(forms.entry),
                    lastEngine_);
    e.before_ = lastEngine_;
    e.opened_ = e.p_.at;
    lastEngine_ = BarrierEngine::Copy;
    encodersOpened_++;
    activeBlit_ = &e;
    if (takeKindSwitch())
        e.crossKinds();
    SWITCH_METAL_DIAGNOSTIC(taken_.count(opened));
    blits++;
    return &e;
}

TextureTransferResult Device::transferTexture(const Texture &texture, uint32_t level, uint32_t slice,
                                              const uint32_t origin[3], const uint32_t size[3], void *bytes,
                                              uint64_t row, uint64_t image, bool write, std::shared_ptr<void> owner)
{
    if (texture.texelRows)
        return level || slice ? TextureTransferResult::Unsupported
                              : transferTexelRows(texture, origin, size, bytes, write, std::move(owner));
    Texture view;
    const Texture *selected = &texture;
    if (texture.chain && level) {  // a chain's level 0 is its own image
        if (!chainLevelImage(texture, level, view))
            return TextureTransferResult::Unsupported;
        selected = &view;
    } else if (level) {
        return TextureTransferResult::Unsupported;
    }
    const Texture &t = *selected;
    ZetaCpuCopy zeta;
    fx::MipChain grid;
    if (!t.va || (texture.zeta && !zetaCpuCopy(texture.format, zeta)) ||
        (t.samples > 1 && (!texture.zeta || !fx::msLayout(t.width, t.height, t.samples, t.bpp, grid))) ||
        t.converted() || t.blockW > 1 || t.blockH > 1 || xrForm(t.format) != XrForm::None || !size[0] || !size[1] ||
        !size[2] || origin[0] > t.width || size[0] > t.width - origin[0] || origin[1] > t.height ||
        size[1] > t.height - origin[1])
        return TextureTransferResult::Unsupported;
    const bool volume = t.type == fx::kTexThreeD;
    if (volume ? (slice || origin[2] > t.depth || size[2] > t.depth - origin[2])
               : (slice >= std::max(1u, t.layers) || origin[2] || size[2] != 1))
        return TextureTransferResult::Unsupported;
    TextureTransfer region;
    region.texture = sideOf(t, origin[0], origin[1], volume ? origin[2] : slice);
    if (t.samples > 1) {  // the pixels' cells of the sample grid
        region.texture = {
            t.va, t.pitch,      origin[0] << grid.sxLog2, origin[1] << grid.syLog2, t.height << grid.syLog2,
            true, t.blockHeight};
        region.samples = t.samples;
        region.sxLog2 = grid.sxLog2;
        region.syLog2 = grid.syLog2;
    }
    region.width = size[0];
    region.height = size[1];
    region.planes = size[2];
    region.bpp = texture.zeta ? zeta.bytes : t.bpp;
    region.remapIn = zeta.remapIn;
    region.remapOut = zeta.remapOut;
    if (!transfers_.copy(k_, set_, lock_, region, bytes, row, image, write, std::move(owner)))
        return TextureTransferResult::Failed;
    SWITCH_METAL_DIAGNOSTIC(
        (write ? stats.textureUploads : stats.textureReadbacks).fetch_add(1, std::memory_order_relaxed));
#if SWITCH_METAL_DIAGNOSTICS
    if (write && glassMemOn.load(std::memory_order_relaxed))
        glassMem->wrote(texture.storageRange().va ? texture.storageRange().va : texture.va,
                        commitCount_.load(std::memory_order_relaxed), stats.memEvents.load(std::memory_order_relaxed));
#endif
    return TextureTransferResult::Complete;
}

bool Device::copyCallerPages(const Texture &t, std::shared_ptr<void> owner)
{
    if (!t.callerPages)
        return true;
    if (!transfers_.copyBytes(k_, set_, lock_, t.callerPages->bo.va, t.imagePitchVa, t.storage.bytes, std::move(owner)))
        return false;
    SWITCH_METAL_DIAGNOSTIC(stats.textureUploads.fetch_add(1, std::memory_order_relaxed));
    return true;
}

// NVK's copies of pitch memory: a byte copy (vkCmdCopyBuffer, nvk_cmd_copy_memory_ce: 128 KiB per launch, no remap) or a
// rectangle of pitch rows (vkCmdCopyImage of linear images, nouveau_copy_rect: remap of min(bpp, 4)-byte components, so the line
// length counts pixels). LAUNCH_DMA: DATA_TRANSFER_TYPE PIPELINED, FLUSH_ENABLE, SRC/DST_MEMORY_LAYOUT PITCH, MULTI_LINE_ENABLE.

void BlitEncoder::room(uint32_t words)
{
    if (p_.room() >= words + 8)
        return;
    checkRoom(p_, "a blit encoder");
    cb_->loops_.closeSegment(stream());  // a new reservation: the copies so far become a segment
    dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_->push_.bo.cpu);
    uint64_t va;
    start_ = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, 1 << 14, 256, va));
    p_ = Push{start_, start_ + (1 << 12)};
    cb_->loops_.openSegment(stream());
}

void BlitEncoder::copyBytes(uint64_t src, uint64_t dst, uint64_t bytes)
{
    cb_->bufferWrites_.note(dst, bytes);
    for (uint64_t done = 0; done < bytes;) {
        room();
        const uint32_t n = uint32_t(std::min<uint64_t>(bytes - done, 1u << 17));
        p_.inc(CE_OFFSET_IN_UPPER,
               {uint32_t((src + done) >> 32), uint32_t(src + done), uint32_t((dst + done) >> 32), uint32_t(dst + done)},
               SUBC_COPY);
        p_.inc(CE_LINE_LENGTH_IN, {n, 1}, SUBC_COPY);
        p_.m(CE_LAUNCH_DMA, kLaunchPitch, SUBC_COPY);
        SWITCH_METAL_DIAGNOSTIC(dev_->stats.ceOps++);
        SWITCH_METAL_DIAGNOSTIC(dev_->stats.ceBytes += n);
        done += n;
    }
}

// SET_REMAP_COMPONENTS: DST_X..W CONST_A (4), COMPONENT_SIZE ONE (0) or FOUR (3) bytes, one source and destination
// component; LAUNCH_DMA: PIPELINED, FLUSH, pitch both sides, REMAP
namespace {
constexpr uint32_t kFillConstA = 0x4444, kFillMaxDim = 1u << 15;
constexpr uint32_t kFillLaunch = 1u | 1u << 2 | 1u << 7 | 1u << 8 | 1u << 10;
}  // namespace

void BlitEncoder::fillRun(uint64_t at, uint32_t width, uint32_t height, [[maybe_unused]] uint32_t unit)
{
    room();
    p_.inc(CE_OFFSET_IN_UPPER + 8, {uint32_t(at >> 32), uint32_t(at)}, SUBC_COPY);
    p_.inc(CE_LINE_LENGTH_IN, {width, height}, SUBC_COPY);
    p_.m(CE_LAUNCH_DMA, kFillLaunch | (height > 1 ? 1u << 9 : 0), SUBC_COPY);
    SWITCH_METAL_DIAGNOSTIC(dev_->stats.ceOps++);
    SWITCH_METAL_DIAGNOSTIC(dev_->stats.ceBytes += uint64_t(width) * height * unit);
}

void BlitEncoder::fillWords(uint64_t dst, uint64_t bytes, uint32_t word)
{
    cb_->bufferWrites_.note(dst, bytes);
    if (bytes < 4)
        return;
    room();
    p_.m(CE_SET_REMAP_CONST_A, word, SUBC_COPY);
    p_.m(CE_SET_REMAP_COMPONENTS, kFillConstA | 3u << 16, SUBC_COPY);
    p_.inc(CE_PITCH_IN, {kFillMaxDim * 4, kFillMaxDim * 4}, SUBC_COPY);
    for (uint64_t at = dst, left = bytes & ~uint64_t(3); left >= 4;) {
        const uint64_t words = left / 4;
        const uint32_t width = uint32_t(std::min<uint64_t>(words, kFillMaxDim)),
                       height =
                           words >= kFillMaxDim ? uint32_t(std::min<uint64_t>(words / kFillMaxDim, kFillMaxDim)) : 1;
        fillRun(at, width, height, 4);
        at += uint64_t(width) * height * 4;
        left -= uint64_t(width) * height * 4;
    }
}

void BlitEncoder::fillBytes(uint64_t dst, uint64_t bytes, uint8_t value)
{
    cb_->bufferWrites_.note(dst, bytes);
    auto bytesOf = [&](uint64_t at, uint64_t n) {
        if (!n)
            return;
        room();
        p_.m(CE_SET_REMAP_CONST_A, value, SUBC_COPY);
        p_.m(CE_SET_REMAP_COMPONENTS, kFillConstA, SUBC_COPY);
        fillRun(at, uint32_t(n), 1, 1);
    };
    const uint64_t end = dst + bytes, first = std::min((dst + 3) & ~uint64_t(3), end),
                   last = std::max(end & ~uint64_t(3), first);
    bytesOf(dst, first - dst);
    if (last > first)
        fillWords(first, last - first, uint32_t(value) * 0x01010101u);
    bytesOf(last, end - last);
}

void BlitEncoder::copyRect(uint64_t src, uint32_t srcPitch, uint64_t dst, uint32_t dstPitch, uint32_t width,
                           uint32_t lines, uint32_t bpp)
{
    cb_->bufferWrites_.note(dst, rowStorageBytes(dstPitch, width, lines, bpp));
    if (!width || !lines || !bpp)
        return;
    copyElements({src, srcPitch}, {dst, dstPitch}, width, lines, texelRemap(bpp), bpp);
}

void BlitEncoder::fillTexels(const Texture &t, uint32_t z, uint32_t w, uint32_t h, const CopyFill &fill)
{
    cb_->use(t, true);
    room(40);
    p_.m(CE_SET_REMAP_CONST_A, fill.constA, SUBC_COPY);
    p_.m(CE_SET_REMAP_CONST_B, fill.constB, SUBC_COPY);
    const CopySide side = sideOf(t, 0, 0, z);
    copyElements(side, side, w, h, fill.remap, uint32_t(fill.componentBytes) * fill.components);
}

void BlitEncoder::fillRect(const Texture &t, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h,
                           const uint8_t texel[16], uint32_t bytes, uint32_t words)
{
    if (!w || !h || !bytes || bytes > 16)
        return;
    cb_->use(t, true);
    const CopySide side = sideOf(t, x, y, z);
    const uint32_t size = std::min(bytes, 4u), count = bytes / size;
    uint32_t value[4] = {};
    for (uint32_t k = 0; k < count; k++)
        memcpy(&value[k], texel + k * size, size);
    // Two constants a launch: a component of a third value waits for the next launch, whose NO_WRITE components keep what
    // the one before wrote. The first written component takes CONST_B, as either constant fills alike: the copy engine's
    // model (drivers/TegraGM20B/test/gm20b_ce_model.h) reads DST_X CONST_A as a fill of every component.
    bool first = true;
    for (uint32_t left = words ? words : (1u << count) - 1; left; first = false) {
        uint32_t remap = 0, a = 0, b = 0, done = 0;
        bool haveA = false, haveB = false;
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t source = kRemapNoWrite;
            if (k < count && (left >> k & 1)) {
                if (!haveB || value[k] == b) {
                    b = value[k], haveB = true, source = kRemapConstB;
                } else if (!haveA || value[k] == a) {
                    a = value[k], haveA = true, source = kRemapConstA;
                }
            }
            if (source != kRemapNoWrite)
                done |= 1u << k;
            remap |= source << (4 * k);
        }
        remap |= (size - 1) << 16 | (count - 1) << 20 | (count - 1) << 24;
        room(40);
        p_.m(CE_SET_REMAP_CONST_A, a, SUBC_COPY);
        p_.m(CE_SET_REMAP_CONST_B, b, SUBC_COPY);
        copyElements(side, side, w, h, remap, bytes, !first);
        left &= ~done;
    }
}

void BlitEncoder::access(const void *read, const void *write)
{
    auto in = [](const void *const *names, uint8_t n, const void *name) {
        return name && std::find(names, names + n, name) != names + n;
    };
    if (in(written_, writes_, read) || in(written_, writes_, write) || in(read_, reads_, write) ||
        reads_ == kAccesses || writes_ == kAccesses) {
        barrier();
        reads_ = writes_ = 0;
    }
    if (read && !in(read_, reads_, read))
        read_[reads_++] = read;
    if (write && !in(written_, writes_, write))
        written_[writes_++] = write;
}

void BlitEncoder::barrier()
{
    room(8);
    p_.m(WAIT_FOR_IDLE, 0);
    p_.inc(CE_LINE_LENGTH_IN, {0, 0}, SUBC_COPY);
    constexpr uint32_t kNonPipelined = 2, kFlush = 1u << 2, kPitchToPitch = 1u << 7 | 1u << 8, kRemap = 1u << 10;
    p_.m(CE_LAUNCH_DMA, kNonPipelined | kFlush | kPitchToPitch | kRemap, SUBC_COPY);
}

void BlitEncoder::crossKinds()
{
    room(SWITCH_GM20B_KIND_SWITCH_WORDS);
    kit(p_, switch_gm20b_kind_switch);
    reads_ = writes_ = 0;
}

void BlitEncoder::timestamp(uint64_t va, bool afterCopies)
{
    room(counters::kTimestampWords);
    cb_->useAddress(va, counters::kRecordBytes, true);
    counters::emitCopy(p_, va, afterCopies);
}

void BlitEncoder::generate(Texture &t, uint32_t fmt2d, uint32_t first, uint32_t last,
                           [[maybe_unused]] uint32_t unproven, uint32_t firstLayer, uint32_t layers, bool point)
{
    Texture::Chain *c = t.chain;
    if (!c || !fmt2d || first >= last || last >= c->layout.levels)
        return;
    cb_->use(t, true);
    room(uint32_t(fx::generateWords(c->layout, first, last) * layers));
    fx::emitGenerateLayers(p_, c->layout, c->va, fmt2d, first, last, firstLayer, layers, point);
    SWITCH_METAL_DIAGNOSTIC(dev_->stats.chainGenerates++);
    SWITCH_METAL_DIAGNOSTIC(dev_->stats.mipTwodLevels += last - first);
    SWITCH_METAL_DIAGNOSTIC(dev_->stats.mipTwodUnproven += unproven);
}

void BlitEncoder::end()
{
    if (cb_->activeBlit_ == this)
        cb_->activeBlit_ = nullptr;
    if (p_.at == opened_)
        cb_->lastEngine_ = before_;
    checkRoom(p_, "a blit encoder");
    cb_->loops_.closeSegment(stream());
    dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_->push_.bo.cpu);
    SWITCH_METAL_DIAGNOSTIC(dev_->stats.pushBytes += uint64_t(p_.at - start_) * 4);
}

}  // namespace gm20b
