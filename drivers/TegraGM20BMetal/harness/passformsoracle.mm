// SPDX-License-Identifier: MIT
// What this Mac's Apple GPU does (API validation off) with the passes the pass-begin sites still send elsewhere: format 263
// and RG8Unorm_sRGB targets, a texture buffer target, depth/stencil attachments that differ in texture, level, slice, size or
// samples, and depth/stencil resolves into a level, a slice, a memoryless texture or another format.
// usage: p3probe <case>   (one case a process: a fault names itself)
// build: xcrun clang++ -std=c++17 -fobjc-arc passformsoracle.mm passformsoracle_colour.mm passformsoracle_zeta.mm
//        -framework Metal -framework Foundation -o p3probe
#include "passformsoracle.h"

using namespace passforms;

int main(int argc, char **argv)
{
    @autoreleasepool {
        gDevice = MTLCreateSystemDefaultDevice();
        gQueue = [gDevice newCommandQueue];
        NSError *e = nil;
        gLibrary = [gDevice newLibraryWithSource:kSource options:nil error:&e];
        if (!gLibrary) {
            printf("library: %s\n", e.description.UTF8String);
            return 1;
        }
        const std::map<std::string, void (*)()> cases = {
            {"zeta_two_textures",
             [] {
                 zetaTwoTextures(true);
                 zetaTwoTextures(false);
             }},
            {"zeta_two_levels",
             [] {
                 zetaTwoLevels(true);
                 zetaTwoLevels(false);
             }},
            {"zeta_two_slices",
             [] {
                 zetaTwoSlices(true);
                 zetaTwoSlices(false);
             }},
            {"pair_sizes", pairSizes},
            {"pair_samples", pairSamples},
            {"mixed_pairs", mixedPairs},
            {"stencil_view", stencilView},
            {"zeta_resolves", zetaResolves},
            {"unspecialized", unspecialized},
            {"rg8_srgb", rg8Srgb},
            {"texture_buffer", textureBuffer},
            {"unspecialized_views", unspecializedViews},
            {"area_store",
             [] {
                 areaStore(64, 40, 24);
                 areaStore(256, 40, 80);
                 areaStore(256, 100, 20);
                 areaStore(256, 40, 80, MTLPixelFormatRGBA16Float);
             }},
        };
        if (argc < 2 || !cases.count(argv[1])) {
            for (const auto &[name, run] : cases)
                printf("%s\n", name.c_str());
            return argc < 2 ? 0 : 1;
        }
        cases.at(argv[1])();
    }
    return 0;
}
