#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../NativeBridgeMetalFrontend.h"
#include "../bridge/NativeMetalDirect.h"
#include "../../TegraGM20BMetal/raw/runtime/raw_direct_compute.h"
#include "../../TegraGM20BMetal/raw/runtime/raw_direct_encode.h"
#include "../../TegraGM20BMetal/raw/runtime/raw_direct_nvk.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

struct Case {
    const char *name;
    bool bound;
    bool sampler;
    bool called = false;
};

static bool record(const NmbDirectKernel *kernel, void *context)
{
    auto &test = *static_cast<Case *>(context);
    test.called = true;
    auto compiler = raw::DirectNvk::find(native_bridge_metal_nvk_symbol);
    raw::ComputeRecord result;
    std::string why;
    if (raw::directKernel(*kernel, compiler, test.name, result, why) != raw::CompileOutcome::Made) {
        fprintf(stderr, "%s: %s\n", test.name, why.c_str());
        return false;
    }
    unsigned images = 0, samplers = 0;
    for (unsigned i = 0; i < kernel->writeCount; ++i) {
        const auto &write = kernel->writes[i].write;
        raw::SetSlot layout;
        if (!raw::Set0(kernel->set0Bindings, kernel->set0BindingCount).slot(write.binding, layout))
            return false;
        const unsigned offset = layout.offset + write.element * layout.stride;
        const auto *begin = result.kernel.entry, *end = begin + result.kernel.entries;
        auto entry = [&](unsigned kind) {
            return std::find_if(begin, end, [&](const auto &e) {
                return e.offset == offset && e.kind == kind;
            });
        };
        if (write.type == raw::SampledImage || write.type == raw::StorageImage ||
            write.type == raw::CombinedImageSampler) {
            ++images;
            const auto *texture = entry(raw::KernelTexture);
            if ((texture != end) != test.bound || (texture != end && texture->slot != 0))
                return false;
            if (!test.bound && write.source != NmbDirectNeutral &&
                !(write.source == NmbDirectCombined && write.slot == NMB_DIRECT_NEUTRAL))
                return false;
        }
        if (write.type == raw::Sampler || write.type == raw::CombinedImageSampler) {
            ++samplers;
            const auto *sampler = entry(raw::KernelSampler);
            if (sampler == end || !(sampler->slot & 0x80) || (sampler->slot & 0x7f) >= result.samplers.size())
                return false;
        }
    }
    return images == 1 && samplers == unsigned(test.sampler) && !result.code.empty();
}

int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    @autoreleasepool {
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0, nullptr);
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        if (!library) {
            fprintf(stderr, "library: %s\n", error.description.UTF8String);
            return 1;
        }
        unsigned failures = 0;
        for (const char *name : {"neutral_read", "neutral_sample", "neutral_store", "neutral_compare"}) {
            for (bool bound : {false, true}) {
                auto *descriptor = [MTLComputePipelineDescriptor new];
                descriptor.computeFunction = [library newFunctionWithName:@(name)];
                bool depth = !strcmp(name, "neutral_compare");
                NmbDirectBoundTexture texture = {0,
                                                 depth ? MTLPixelFormatDepth32Float : MTLPixelFormatRGBA32Float,
                                                 MTLTextureType2D,
                                                 MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite,
                                                 1,
                                                 1,
                                                 1};
                NmbDirectDispatch dispatch = {};
                dispatch.threads[0] = 32;
                dispatch.threads[1] = dispatch.threads[2] = 1;
                dispatch.textures = bound ? &texture : nullptr;
                dispatch.textureCount = bound ? 1 : 0;
                Case test = {name, bound, depth || !strcmp(name, "neutral_sample")};
                const char *why = nullptr;
                bool made = native_bridge_metal_compute_direct(descriptor, &dispatch, record, &test, &why);
                printf("%s %s texture=%s sampler=unbound%s%s\n", made && test.called ? "PASS" : "FAIL", name,
                       bound ? "bound" : "unbound", why ? " reason=" : "", why ?: "");
                failures += !made || !test.called;
            }
        }
        return failures ? 1 : 0;
    }
}
