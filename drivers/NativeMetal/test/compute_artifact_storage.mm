// SPDX-License-Identifier: MIT
#include "../bridge/NativeMetalObjects.h"
#include "../bridge/NativeMetalComputeArtifact.h"
#include <atomic>

static unsigned creations = 0;
static bool denyCreation = false;

static NmbComputeArtifact *trackedArtifactNew(NmbDevice *device, const NmbComputePipelineDescriptor *descriptor,
                                              bool reflection)
{
    ++creations;
    return denyCreation ? nullptr : nmb_compute_artifact_new(device, descriptor, reflection);
}

#define nmb_compute_artifact_new trackedArtifactNew
#include "../NativeBridgeMetalFrontend.mm"
#undef nmb_compute_artifact_new

static void missingStorageSelector(const char *type, const char *selector)
{
    fprintf(stderr, "MISSING %s %s\n", type, selector);
}

int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    @autoreleasepool {
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0, missingStorageSelector);
        if (!device)
            return 1;
        gNativeArtifactStorage.cache.clear();
        gNativeArtifactStorage.seed.clear();
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        auto *descriptor = [MTLComputePipelineDescriptor new];
        descriptor.computeFunction = [library newFunctionWithName:@"artifact_probe"];
        descriptor.maxTotalThreadsPerThreadgroup = 128;
        unsigned failures = 0;
        const auto check = [&](bool good, const char *name) {
            printf("%s %s\n", good ? "PASS" : "FAIL", name);
            failures += !good;
        };
        const auto prepare = [&](MTLComputePipelineDescriptor *input, bool reflected) -> id {
            id reflection = nil;
            return native_bridge_metal_compute_prepare(
                input, reflected ? MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo : 0,
                reflected ? &reflection : nullptr, &error);
        };
        id plain = prepare(descriptor, false), reflected = prepare(descriptor, true);
        check(plain && reflected && creations == 2, "cold controls execute real AIR analysis");
        if (!plain || !reflected)
            return 1;
        const std::string seed = std::string(argv[2]) + "/fixture-seed";
        char plainName[69] = {}, reflectionName[69] = {};
        check(native_bridge_metal_compute_save(plain, seed.c_str(), plainName, sizeof plainName) &&
                  native_bridge_metal_compute_save(reflected, seed.c_str(), reflectionName, sizeof reflectionName) &&
                  strcmp(plainName, reflectionName),
              "export both reflection identities");
        gNativeArtifactStorage.seed = seed;
        denyCreation = true;
        MTLComputePipelineDescriptor *same = [descriptor copy];
        same.label = @"a different label";
        id plainHit = prepare(same, false), reflectionHit = prepare(same, true);
        check(plainHit && reflectionHit && creations == 2, "seed imports while AIR constructor is disabled");
        check([plainHit maxTotalThreadsPerThreadgroup] == [plain maxTotalThreadsPerThreadgroup] &&
                  [reflectionHit staticThreadgroupMemoryLength] == [reflected staticThreadgroupMemoryLength],
              "imported public pipeline properties agree");
        same.maxTotalThreadsPerThreadgroup = 64;
        check(!prepare(same, false) && creations == 3, "changed descriptor cannot reuse an unrelated artifact");
        same.maxTotalThreadsPerThreadgroup = 128;
        const std::string process = std::string(argv[2]) + "/fixture-process";
        char copiedName[69] = {};
        check(native_bridge_metal_compute_save(plain, process.c_str(), copiedName, sizeof copiedName),
              "export process cache record");
        gNativeArtifactStorage.cache = process;
        gNativeArtifactStorage.seed.clear();
        check(prepare(same, false) && creations == 3, "private cache imports without seed or AIR analysis");
        const std::string path = process + "/" + plainName;
        NSMutableData *corrupt = [NSMutableData dataWithContentsOfFile:@(path.c_str())];
        if (!corrupt.length)
            return 1;
        static_cast<uint8_t *>(corrupt.mutableBytes)[corrupt.length - 1] ^= 1;
        check([corrupt writeToFile:@(path.c_str()) atomically:YES], "damage private cache control");
        gNativeArtifactStorage.seed = seed;
        check(prepare(same, false) && creations == 3, "damaged private record falls through to valid seed");
        gNativeArtifactStorage.seed.clear();
        check(!prepare(same, false) && creations == 4, "damaged record cannot masquerade as a hit");
        gNativeArtifactStorage.cache.clear();
        denyCreation = false;
        check(prepare(same, false) && creations == 5, "cache miss can perform fresh analysis");
        const std::string background = std::string(argv[2]) + "/fixture-background";
        gNativeArtifactStorage.cache = background;
        dispatch_semaphore_t permit = dispatch_semaphore_create(0);
        dispatch_async(persistQueue(), ^{
            dispatch_semaphore_wait(permit, DISPATCH_TIME_FOREVER);
        });
        __weak id retained = nil;
        @autoreleasepool {
            MTLComputePipelineDescriptor *queued = [same copy];
            queued.maxTotalThreadsPerThreadgroup = 96;
            retained = prepare(queued, false);
            objc_setAssociatedObject(queued, &computeArtifactKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
        check(retained && creations == 6 && gNativeArtifactWrites == 1,
              "background persistence retains artifact after caller release without waiting");
        gNativeArtifactStorage.cache.clear();
        dispatch_semaphore_signal(permit);
        dispatch_sync(persistQueue(), ^{
                      });
        check(gNativeArtifactWrites == 0, "background persistence retires its bounded slot");
        same.maxTotalThreadsPerThreadgroup = 96;
        gNativeArtifactStorage.cache = background;
        denyCreation = true;
        check(prepare(same, false) && creations == 6,
              "background record imports from captured directory without fresh AIR analysis");
        return failures ? 1 : 0;
    }
}
