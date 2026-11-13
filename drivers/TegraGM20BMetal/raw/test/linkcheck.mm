// SPDX-License-Identifier: MIT
// linkcheck: a render state whose fragment function calls a visible function the pipeline links (MTLLinkedFunctions, as
// iconservicesagent's custom effects do) made by the direct compile (runtime/RawDirect.h). Its record must be the one of the
// same function with the callee inline (the translator links the callee's AIR and inlines it), and the state without its linked
// function refused.
// usage: linkcheck <nvk dylib> <cache dir> <pack.bin> <caller.metallib> <callee.metallib> (test/linked/*.metal)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include "../runtime/RawDirect.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#include "record_compare.h"

namespace {

constexpr const char *kKey = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0";

void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

MTLRenderPipelineDescriptor *descriptor(id<MTLLibrary> caller, NSString *fragment, id<MTLFunction> linked)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [caller newFunctionWithName:@"linked_vert"];
    d.fragmentFunction = [caller newFunctionWithName:fragment];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    if (linked) {
        MTLLinkedFunctions *functions = [MTLLinkedFunctions linkedFunctions];
        functions.functions = @[ linked ];
        d.fragmentLinkedFunctions = functions;
    }
    return d;
}

bool sameProgram(const raw::CapturedProgram &a, const raw::CapturedProgram &b)
{
    return a.image == b.image && a.data == b.data && a.cbufTypes == b.cbufTypes;
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 6) {
            fprintf(stderr,
                    "usage: linkcheck <nvk dylib> <cache dir> <pack.bin> <caller.metallib> <callee.metallib>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        raw::Pack pack;
        std::string err;
        if (!dev || !pack.load(argv[3], 0, err)) {
            fprintf(stderr, "linkcheck: %s\n", dev ? err.c_str() : nmb_last_error());
            return 1;
        }
        NSError *error = nil;
        id<MTLLibrary> caller = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[4])] error:&error];
        id<MTLLibrary> callee = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[5])] error:&error];
        id<MTLFunction> custom = [callee newFunctionWithName:@"custom_fn"];
        if (!caller || !custom) {
            fprintf(stderr, "linkcheck: libraries unread\n");
            return 1;
        }
        raw::Direct direct(pack);
        raw::RuntimeRecord linked, inlined, unlinked;
        std::string whyLinked, whyInlined, whyUnlinked;
        const raw::CompileOutcome made =
            direct.make(descriptor(caller, @"linked_frag", custom), kKey, linked, whyLinked);
        const raw::CompileOutcome inl = direct.make(descriptor(caller, @"inline_frag", nil), kKey, inlined, whyInlined);
        const raw::CompileOutcome alone =
            direct.make(descriptor(caller, @"linked_frag", nil), kKey, unlinked, whyUnlinked);
        int failed = 0;
        if (made != raw::CompileOutcome::Made || inl != raw::CompileOutcome::Made) {
            printf("FAIL: not made (linked: %s; inline: %s)\n", whyLinked.c_str(), whyInlined.c_str());
            return 1;
        }
        if (!sameProgram(linked.programs[1], inlined.programs[1])) {
            printf("FAIL: the linked state's fragment program is not the inline one's (%zu and %zu bytes)\n",
                   linked.programs[1].image.size(), inlined.programs[1].image.size());
            failed = 1;
        }
        if (!sameProgram(linked.programs[0], inlined.programs[0]) || linked.blob != inlined.blob ||
            memcmp(linked.pipeline.entry, inlined.pipeline.entry, sizeof linked.pipeline.entry) ||
            linked.pipeline.entries != inlined.pipeline.entries) {
            printf("FAIL: the linked state's record is not the inline one's\n");
            failed = 1;
        }
        if (alone != raw::CompileOutcome::Refused || whyUnlinked.find("LinkedFunction") == std::string::npos) {
            printf("FAIL: the state without its linked function was not refused for it (%s)\n", whyUnlinked.c_str());
            failed = 1;
        }
        printf("linkcheck: linked state made, fragment program %zu bytes, %s the inline function's; unlinked refused: "
               "%s\n",
               linked.programs[1].image.size(), failed ? "NOT" : "byte for byte", whyUnlinked.c_str());
        return failed;
    }
}
