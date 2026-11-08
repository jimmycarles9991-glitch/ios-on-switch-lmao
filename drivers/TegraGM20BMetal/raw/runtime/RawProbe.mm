// SPDX-License-Identifier: MIT
// See RawProbe.h. Its inputs and attachments are pack/probe.mm's, one for one: a record is the generator's only for the same draw.
#import "RawProbe.h"

#include "../pack/nvdec.h"
#include "raw_pack.h"

#include "NativeBridgeMetalFrontend.h"

// Weak, so the host benchmark links without the frontend
extern "C" bool native_bridge_metal_capture(id commandBuffer,
                                            void (^segment)(uint64_t addr, const uint32_t *cpu, uint32_t dwords),
                                            NBCaptureContext *context) __attribute__((weak_import));
extern "C" const void *native_bridge_metal_capture_map(id commandBuffer, uint64_t va, uint64_t bytes)
    __attribute__((weak_import));
extern "C" bool native_bridge_metal_capture_program(id commandBuffer, uint32_t program, NBCapturedProgram *out)
    __attribute__((weak_import));

namespace raw {
namespace {

constexpr uint32_t kSlots = 31;   // probe.mm PROBE_SLOTS' default: Metal's whole buffer table
constexpr uint32_t kTagged = 16;  // fragment texture and sampler slots
constexpr uint32_t kProbeIndex =
    0x5717;  // the payloads' probe index (layouts.py checks a pointer's tag names its probe)
constexpr uint32_t kTargetSide = 64;
constexpr uint32_t kPayloads[3] = {256, 16, 4096};

// probe.mm's: a 542 snapshot renders into the frontend's RGBA16Float stand-in, and an extended-range attachment (550 to 555) is
// held as its raw codes; a load or store of either adds m2v's own passes, so the state's draw would not be the pass's only one
constexpr MTLPixelFormat kRGB8_444_2P = MTLPixelFormat(542);

bool extendedRange(MTLPixelFormat f)
{
    return f >= MTLPixelFormat(550) && f <= MTLPixelFormat(555);
}

bool operator!=(const InitAddresses &a, const InitAddresses &b)
{
    return a.heap != b.heap || a.zero != b.zero || a.root != b.root || a.tic != b.tic || a.tsc != b.tsc ||
           a.ticMax != b.ticMax || a.tscMax != b.tscMax;
}

IOSurfaceRef newSurface(uint32_t w, uint32_t h)
{
    NSDictionary *p = @{
        (id)kIOSurfaceWidth : @(w),
        (id)kIOSurfaceHeight : @(h),
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @(w * 4),
        (id)kIOSurfacePixelFormat : @((uint32_t)'BGRA')
    };
    return IOSurfaceCreate((__bridge CFDictionaryRef)p);
}

}  // namespace

Probe::Probe(id<MTLDevice> device, const Pack &pack) : device_(device), pack_(pack)
{
    queue_ = [device newCommandQueue];
    NSMutableArray<id<MTLTexture>> *textures = [NSMutableArray array];
    for (uint32_t t = 0; t < kTagged; t++) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:16 + t
                                                                                     height:8 + t
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        if (id<MTLTexture> texture = [device newTextureWithDescriptor:td])
            [textures addObject:texture];
    }
    textures_ = textures;
    NSMutableArray<id<MTLSamplerState>> *samplers = [NSMutableArray array];
    for (uint32_t s = 0; s < kTagged; s++) {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = s & 1 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.mipFilter = MTLSamplerMipFilterNearest;
        sd.sAddressMode = s & 2 ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
        sd.lodMaxClamp = 1 + 0.5f * s;
        if (id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sd])
            [samplers addObject:sampler];
    }
    samplers_ = samplers;
    framebufferSurface_ = newSurface(kTargetSide, kTargetSide);
    MTLTextureDescriptor *fd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:kTargetSide
                                                                                 height:kTargetSide
                                                                              mipmapped:NO];
    fd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    fd.storageMode = MTLStorageModeShared;
    framebuffer_ =
        framebufferSurface_ ? [device newTextureWithDescriptor:fd iosurface:framebufferSurface_ plane:0] : nil;
    targets_ = [NSMutableDictionary dictionary];
    capture_ = std::make_unique<Capture>(
        [this](uint64_t va, uint64_t bytes) {
            return static_cast<const uint8_t *>(native_bridge_metal_capture_map(captured_, va, bytes));
        },
        [this](uint32_t offset, nvdec::ProgramInfo &out) {
            NBCapturedProgram p = {};
            if (!native_bridge_metal_capture_program(captured_, offset, &p) || p.headerBytes != 80)
                return false;
            const auto *header = static_cast<const uint8_t *>(p.header), *code = static_cast<const uint8_t *>(p.code);
            const auto *data = static_cast<const uint8_t *>(p.data);
            out.image.assign(header, header + p.headerBytes);
            out.image.insert(out.image.end(), code, code + p.codeBytes);
            out.data.assign(data, data + p.dataBytes);
            out.cbufTypes.assign(p.cbufTypes, p.cbufTypes + std::min<uint32_t>(p.cbufCount, 16));
            return true;
        });
}

Probe::~Probe()
{
    if (framebufferSurface_)
        CFRelease(framebufferSurface_);
}

// probe.mm's target(): one 64x64 texture per format, memoryless for a writeMask-0 attachment (Core Animation's second)
id<MTLTexture> Probe::target(MTLPixelFormat format, bool memoryless)
{
    NSString *k = [NSString stringWithFormat:@"%lu.%d", (unsigned long)format, memoryless];
    if (!targets_[k]) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                      width:kTargetSide
                                                                                     height:kTargetSide
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | (memoryless ? 0 : MTLTextureUsageShaderRead);
        td.storageMode = memoryless ? MTLStorageModeMemoryless : MTLStorageModePrivate;
        id<MTLTexture> t = [device_ newTextureWithDescriptor:td];
        if (!t && memoryless) {
            td.storageMode = MTLStorageModePrivate;
            t = [device_ newTextureWithDescriptor:td];
        }
        if (t)
            targets_[k] = t;
    }
    return targets_[k];
}

// probe.mm's pass: the first written BGRA8 color is the IOSurface (Core Animation's framebuffer) unless the state has depth or
// stencil (beside them a linear color is m2v's stand-in, which NVK renders in place only for private ones); memoryless, 542
// and extended-range attachments neither loaded nor stored
MTLRenderPassDescriptor *Probe::pass(MTLRenderPipelineDescriptor *d)
{
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    const bool depthStencil = d.depthAttachmentPixelFormat != MTLPixelFormatInvalid ||
                              d.stencilAttachmentPixelFormat != MTLPixelFormatInvalid;
    bool usedFramebuffer = depthStencil;
    for (NSUInteger c = 0; c < 8; c++) {
        MTLRenderPipelineColorAttachmentDescriptor *ca = d.colorAttachments[c];
        if (ca.pixelFormat == MTLPixelFormatInvalid)
            continue;
        const bool memoryless = ca.writeMask == 0;
        id<MTLTexture> t = (!memoryless && !usedFramebuffer && ca.pixelFormat == MTLPixelFormatBGRA8Unorm)
                               ? framebuffer_
                               : target(ca.pixelFormat, memoryless);
        usedFramebuffer |= t == framebuffer_;
        pass.colorAttachments[c].texture = t;
        const bool loose = memoryless || ca.pixelFormat == kRGB8_444_2P || extendedRange(ca.pixelFormat);
        pass.colorAttachments[c].loadAction = loose ? MTLLoadActionDontCare : MTLLoadActionLoad;
        pass.colorAttachments[c].storeAction = loose ? MTLStoreActionDontCare : MTLStoreActionStore;
    }
    if (depthStencil && d.depthAttachmentPixelFormat != MTLPixelFormatInvalid)
        pass.depthAttachment.texture = target(d.depthAttachmentPixelFormat, true);
    if (d.stencilAttachmentPixelFormat != MTLPixelFormatInvalid)
        pass.stencilAttachment.texture = target(d.stencilAttachmentPixelFormat, true);
    return pass;
}

Compiler::Outcome Probe::run(id<MTLRenderPipelineState> twin, MTLRenderPipelineDescriptor *d, uint32_t bytes,
                             nvdec::ProbeSubmission &out, std::string &why, bool bindOffsets)
{
    @autoreleasepool {
        if (bindOffsets && !payloads_) {
            NSMutableArray<id<MTLBuffer>> *made = [NSMutableArray array];
            for (uint32_t k = 0; k < 2 * kSlots; k++)
                if (id<MTLBuffer> b = [device_ newBufferWithLength:256 + bytes options:MTLResourceStorageModeShared])
                    [made addObject:b];
            if (made.count != 2 * kSlots) {
                why = "the probe's payload buffers were not made";
                return Compiler::Outcome::Failed;
            }
            payloads_ = made;
        }
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass(d)];
        [enc setRenderPipelineState:twin];
        std::vector<uint32_t> tag(bytes / 4);
        for (uint32_t stage = 1; stage <= 2; stage++)
            for (uint32_t slot = 0; slot < kSlots; slot++) {
                tag[0] = 0xC0DE0000u | stage << 8 | slot;
                tag[1] = kProbeIndex;
                for (uint32_t k = 2; k < tag.size(); k++)
                    tag[k] = 0xA0000000u | stage << 20 | slot << 16 | k;
                if (bindOffsets) {
                    const NSUInteger at = probeBindOffset(stage, slot);
                    id<MTLBuffer> b = payloads_[(stage - 1) * kSlots + slot];
                    memcpy(static_cast<uint8_t *>(b.contents) + at, tag.data(), bytes);
                    if (stage == 1)
                        [enc setVertexBuffer:b offset:at atIndex:slot];
                    else
                        [enc setFragmentBuffer:b offset:at atIndex:slot];
                } else if (stage == 1) {
                    [enc setVertexBytes:tag.data() length:bytes atIndex:slot];
                } else {
                    [enc setFragmentBytes:tag.data() length:bytes atIndex:slot];
                }
            }
        for (NSUInteger t = 0; t < textures_.count; t++)
            [enc setFragmentTexture:textures_[t] atIndex:t];
        for (NSUInteger s = 0; s < samplers_.count; s++)
            [enc setFragmentSamplerState:samplers_[s] atIndex:s];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [enc endEncoding];

        __block std::vector<std::pair<const uint32_t *, uint64_t>> segments;
        __block bool unmapped = false;
        NBCaptureContext context = {};
        captured_ = cb;
        const bool read = native_bridge_metal_capture(
            cb,
            ^(uint64_t, const uint32_t *cpu, uint32_t dwords) {
                unmapped |= !cpu;
                if (cpu)
                    segments.push_back({cpu, dwords});
            },
            &context);
        if (!read || unmapped) {
            why = read ? "NVK recorded a push segment without a CPU view" : "m2v refused the capture of its commands";
            captured_ = nil;
            return Compiler::Outcome::Refused;
        }
        const InitAddresses here = {context.shaderHeap, context.zeroPage, context.cb0,       context.images,
                                    context.samplers,   context.imageMax, context.samplerMax};
        if (!primed_ || here != primedWith_) {
            primed_ = capture_->prime(pack_, here, why);
            primedWith_ = here;
        }
        if (primed_)
            out = capture_->decode(segments);
        captured_ = nil;
        return primed_ ? Compiler::Outcome::Made : Compiler::Outcome::Failed;
    }
}

Compiler::Outcome Probe::make(id<MTLRenderPipelineState> twin, MTLRenderPipelineDescriptor *descriptor,
                              const std::string &key, RuntimeRecord &out, std::string &why)
{
    if (!native_bridge_metal_capture || !native_bridge_metal_capture_map || !native_bridge_metal_capture_program) {
        why = "the frontend has no capture";
        return Compiler::Outcome::Failed;
    }
    if (textures_.count != kTagged || samplers_.count != kTagged || !framebuffer_ || !twin) {
        why = "the probe's inputs were not made";
        return Compiler::Outcome::Failed;
    }
    nvdec::ProbeSubmission runs[4];
    for (int i = 0; i < 4; i++)
        if (const Compiler::Outcome o = run(twin, descriptor, kPayloads[i % 3], runs[i], why, i == 3);
            o != Compiler::Outcome::Made)
            return o;
    if (!rulesMade_) {
        rules_ = PackRules::of(pack_, capture_->context());
        rulesMade_ = true;
    }
    if (makeRecord(
            {&runs[0], &runs[1], &runs[2], kProbeIndex, key, &runs[3]}, rules_,
            [this](const std::string &id) {
                return capture_->program(id);
            },
            out, why))
        return Compiler::Outcome::Made;
    // NVK's export names every program a draw of the command buffer binds: one it does not is the export's failure. A set-0
    // word the probe's tags don't name is this process's (m2v's descriptor tables moved under the capture), not the state's:
    // tried again, never kept on disk as the state's refusal
    const bool ours =
        why == "unknown program" || why.rfind("unidentified handle", 0) == 0 || why.rfind("untagged pointer", 0) == 0;
    return ours ? Compiler::Outcome::Failed : Compiler::Outcome::Refused;
}

}  // namespace raw
