// SPDX-License-Identifier: MIT
// See render_encoder.h (prepareShaderTests: a shader depth/stencil test's stand-ins, filled from the pass's
// attachment before its draws and drained back after them).
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

bool RenderEncoder::prepareShaderTests(const Pipeline &pipeline, const char **why)
{
    const PackShaderTests &tests = pipeline.rec->shaderTests;
    const uint8_t flags = tests.flags;
    const bool shadow = shadowStencil(tests) && flags == kShaderTestStencil;
    if (shadow != shadowStencil_) {
        shadowStencil_ = shadow;
        dsDirty_ = true;
    }
    if (!flags) {
        endShaderTests();
        return true;
    }
    const Texture *zeta = pass_.depthStencil.texture;
    ShaderTestShape copy;
    if (!zeta || zeta->samples != samples_ ||
        !shaderTestShape(zeta->format, samples_, planeArea().x1, planeArea().y1, flags, copy)) {
        // the record's tests are its state's formats' (the key's ds=): a pass without those aspects or of another
        // sample count is the client's; what else the copy refuses is a Depth16Unorm its pass did not convert
        // (TegraGM20BMetalRangeForms.mm depth16Tested) or a record or area past the copy's sizes
        const bool held = zeta && zeta->samples == samples_ &&
                          (!(flags & kShaderTestDepth) || zetaHasDepth(zeta->format)) &&
                          (!(flags & kShaderTestStencil) || zetaHasStencil(zeta->format));
        *why = held ? "a shader depth/stencil test of an attachment the copy takes no form of"
                    : "a shader depth/stencil test of an aspect or sample count the pass's attachment does not have";
        cb_->refusal = {held ? RefTestsInvariant : RefDepthStencil, zeta ? zeta->format : 0, flags, samples_};
        return false;
    }
    if (shadow && shadowDependsOnDepth(ds_, zetaHasDepth(zeta->format))) {
        *why = "a shader-side stencil whose stencil write depends on the depth test after its program, drawn without "
               "its StencilWrite variant";
        cb_->refusal = {RefTestsInvariant, zeta->format, flags, ds_.depthCompare};
        return false;
    }
    // a shader-side stencil's depth test is the hardware's, against the attachment
    if (shadow && (shaderTestsActive_ & kShaderTestDepth)) {
        copyShaderTests(kShaderTestDepth, true);
        shaderTestsActive_ &= uint8_t(~kShaderTestDepth);
        dsDirty_ = true;
    }
    const uint8_t missing = flags & ~shaderTestsActive_;
    if (!missing)
        return true;
    auto obtain = [&](Texture *&slot, uint32_t format, uint32_t tag) {
        if (slot)
            return true;
        Texture shape;
        shape.width = zeta->width;
        shape.height = zeta->height;
        shape.format = format;
        shape.samples = samples_;
        shape.layers = std::max(pass_.layers, 1u);
        if (!(slot = dev_->lastBacking(shape, tag, true, false))) {
            Device::DeferredFrees deferred(*dev_);
            std::unique_lock<gm20b::Mutex> lock(dev_->lock_);
            SWITCH_METAL_ENCODE_COUNT(Locks);
            slot = dev_->backing(lock, deferred, shape, tag, true, false);
        }
        if (!slot)
            return false;
        cb_->backings_.push_back(slot);
        cb_->use(*slot, true);
        invalidateDescriptors();
        return true;
    };
    constexpr uint32_t kR32Float = 55, kR8Uint = 13, kRG8Unorm = 30;
    if (((missing & kShaderTestDepth) && !obtain(shaderDepth_, kR32Float, Device::kShaderDepthTag)) ||
        ((missing & kShaderTestStencil) && !obtain(shaderStencil_, kR8Uint, Device::kShaderStencilTag)) ||
        ((missing & kShaderTestStencil) && copy.packedStencil &&
         !obtain(shaderStencilTemp_, kRG8Unorm, Device::kShaderStencilTempTag))) {
        *why = "no memory for a shader depth/stencil test's stand-ins";
        cb_->refusal = {RefTestsMemory, zeta->format, flags, samples_};
        return false;
    }
    copyShaderTests(missing, false);
    shaderTestsActive_ |= missing;
    dsDirty_ = true;
    return true;
}

void RenderEncoder::copyShaderTests(uint8_t flags, bool toAttachment)
{
    if (p_.room() < 256)
        reserve();
    const Texture &zeta = *pass_.depthStencil.texture;
    ShaderTestShape shape;
    shaderTestShape(zeta.format, samples_, planeArea().x1, planeArea().y1, flags, shape);
    auto side = [](const Texture &texture, uint32_t layer) {
        auto result = sideOf(texture, 0, 0, layer);
        result.height = sampleRows(texture);
        return result;
    };
    cb_->use(zeta, toAttachment);
    tiledCacheDrain(p_, dev_->k_.info());
    p_.m(WAIT_FOR_IDLE, 0);
    if (toAttachment)
        p_.m(INVALIDATE_SHADER_CACHES, kFlushData | kInvalidateData);
    const auto copies = shaderTestTransfers(flags, shape.packedStencil, toAttachment);
    // every layer of a layered pass: a slice-choosing state's tests read the stand-ins' layers as a 2D array
    for (uint32_t layer = 0; layer < std::max(pass_.layers, 1u); ++layer) {
        const auto image = [&](ShaderTestImage which) {
            switch (which) {
            case ShaderTestImage::depth:
                return side(*shaderDepth_, layer);
            case ShaderTestImage::stencil:
                return side(*shaderStencil_, layer);
            case ShaderTestImage::temporary:
                return side(*shaderStencilTemp_, layer);
            case ShaderTestImage::attachment:
                break;
            }
            return side(zeta, layer);
        };
        for (uint8_t at = 0; at < copies.count; ++at) {
            const auto &copy = copies.transfers[at];
            copyRemapped(p_, image(copy.source), image(copy.destination), shape.width, shape.height, copy.remap,
                         copy.bytes, dev_->stats, copy.ordered);
        }
    }
    SWITCH_METAL_DIAGNOSTIC(dev_->stats.compZetaCopies += zeta.comptags);
    p_.inc(CE_LINE_LENGTH_IN, {0, 0}, SUBC_COPY);
    p_.m(CE_LAUNCH_DMA, (kLaunchPitch & ~3u) | 2u | (1u << 10), SUBC_COPY);
    p_.m(INVALIDATE_SHADER_CACHES, kInvalidateData);
    p_.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
}

void RenderEncoder::endShaderTests()
{
    if (!shaderTestsActive_)
        return;
    copyShaderTests(shaderTestsActive_, true);
    shaderTestsActive_ = 0;
    dsDirty_ = true;
}

}  // namespace gm20b
