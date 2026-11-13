// SPDX-License-Identifier: MIT
#include "../NativeBridgeMetalFrontend.mm"
#include "../../TegraGM20BMetal/raw/runtime/RawDirect.h"
#include "../../TegraGM20BMetal/raw/runtime/raw_cache.h"
#include "../../TegraGM20BMetal/raw/runtime/raw_emit.h"
#include <CommonCrypto/CommonDigest.h>
#include <array>
#include <thread>

static bool collectArtifact(const void *data, size_t length, void *context)
{
    auto &out = *static_cast<std::vector<uint8_t> *>(context);
    out.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + length);
    return true;
}

static void checksumArtifact(std::vector<uint8_t> &bytes)
{
    CC_SHA256(bytes.data(), CC_LONG(bytes.size() - 32), bytes.data() + bytes.size() - 32);
}

static uint32_t artifactWord(const std::vector<uint8_t> &bytes, size_t offset)
{
    uint32_t out = 0;
    for (unsigned i = 0; i < 4; ++i)
        out |= uint32_t(bytes.at(offset + i)) << (8 * i);
    return out;
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
        if (!library)
            return 2;
        unsigned failures = 0;
        auto check = [&](bool good, const char *name) {
            printf("%s %s\n", good ? "PASS" : "FAIL", name);
            if (!good)
                fprintf(stderr, "%s\n", nmb_last_error());
            failures += !good;
        };
        raw::PackRules rules;
        rules.varying = raw::emittableMethods();
        raw::Direct direct(rules);
        for (const char *function : {"semantic_extended", "semantic_barrier", "semantic_texture", "artifact_probe"}) {
            auto *descriptor = [MTLComputePipelineDescriptor new];
            descriptor.computeFunction = [library newFunctionWithName:@(function)];
            descriptor.maxTotalThreadsPerThreadgroup = 128;
            NmbComputePipelineDescriptor input = {};
            std::vector<NmbFunction *> linked;
            if (!computeDescriptor(descriptor, &input, linked, &error))
                return 2;
            std::array<uint8_t, 32> key = {}, other = {};
            check(nmb_compute_artifact_key(gDevice, &input, true, key.data()) &&
                      nmb_compute_artifact_key(gDevice, &input, true, other.data()) && key == other,
                  "decode-free key stable");
            auto *original = nmb_compute_artifact_new(gDevice, &input, true);
            if (!original)
                return 2;
            std::vector<uint8_t> bytes;
            check(nmb_compute_artifact_serialize(original, collectArtifact, &bytes), "complete artifact export");
            if (bytes.empty())
                return 2;
            [[NSData dataWithBytes:bytes.data() length:bytes.size()]
                writeToFile:[@(argv[2])
                                stringByAppendingPathComponent:[@(function) stringByAppendingString:@".artifact"]]
                 atomically:YES];
            NmbComputeArtifactInfo before = {}, after = {};
            nmb_compute_artifact_info(original, &before);
            auto *imported = nmb_compute_artifact_import(gDevice, &input, true, bytes.data(), bytes.size());
            check(imported && nmb_compute_artifact_info(imported, &after) && !memcmp(&before, &after, sizeof before),
                  "imported Metal metadata matches");
            if (!imported)
                return 2;
            std::vector<uint8_t> roundtrip;
            check(nmb_compute_artifact_serialize(imported, collectArtifact, &roundtrip) && bytes == roundtrip,
                  "all binding and recursive reflection data roundtrip exactly");
            std::array<bool, 8> concurrent = {};
            std::vector<std::thread> workers;
            for (unsigned i = 0; i < concurrent.size(); ++i)
                workers.emplace_back([&, i] {
                    auto *artifact = nmb_compute_artifact_import(gDevice, &input, true, bytes.data(), bytes.size());
                    std::vector<uint8_t> output;
                    concurrent[i] = artifact && nmb_compute_artifact_serialize(artifact, collectArtifact, &output) &&
                                    output == bytes;
                    nmb_compute_artifact_release(artifact);
                });
            for (auto &worker : workers)
                worker.join();
            check(std::all_of(concurrent.begin(), concurrent.end(),
                              [](bool value) {
                                  return value;
                              }),
                  "concurrent immutable imports");
            auto rejected = [&](const std::vector<uint8_t> &payload, bool reflection) {
                auto *artifact =
                    nmb_compute_artifact_import(gDevice, &input, reflection, payload.data(), payload.size());
                const bool refused = !artifact;
                nmb_compute_artifact_release(artifact);
                return refused;
            };
            check(rejected(bytes, false), "reflection contract mismatch refused");
            auto *plain = nmb_compute_artifact_new(gDevice, &input, false);
            std::vector<uint8_t> plainBytes;
            const bool plainExported = plain && nmb_compute_artifact_serialize(plain, collectArtifact, &plainBytes);
            auto *plainImported = plainExported ? nmb_compute_artifact_import(gDevice, &input, false, plainBytes.data(),
                                                                              plainBytes.size())
                                                : nullptr;
            NmbComputeArtifactInfo plainInfo = {};
            check(plainImported && nmb_compute_artifact_info(plainImported, &plainInfo) && !plainInfo.bindings,
                  "unrequested reflection stays absent on import");
            nmb_compute_artifact_release(plainImported);
            nmb_compute_artifact_release(plain);
            input.max_threads = 64;
            check(nmb_compute_artifact_key(gDevice, &input, true, other.data()) && key != other &&
                      rejected(bytes, true),
                  "descriptor identity mismatch refused");
            input.max_threads = 128;
            auto damaged = bytes;
            damaged[damaged.size() / 2] ^= 1;
            check(rejected(damaged, true), "checksum corruption refused");
            damaged = bytes;
            damaged.resize(damaged.size() - 1);
            check(rejected(damaged, true), "truncation refused");
            const size_t program = 44 + artifactWord(bytes, 40);
            if (artifactWord(bytes, program)) {
                damaged = bytes;
                const size_t first = program + 4 + 24 + 16;
                std::fill(damaged.begin() + first, damaged.begin() + first + 4, 0xff);
                checksumArtifact(damaged);
                check(rejected(damaged, true), "rechecksummed native record range refused");
            }
            const size_t reflection = program + 4 + artifactWord(bytes, program);
            damaged = bytes;
            const size_t rootChild = reflection + 4 + 4 + 8 + 64;
            std::fill(damaged.begin() + rootChild, damaged.begin() + rootChild + 4, 0);
            checksumArtifact(damaged);
            check(rejected(damaged, true), "rechecksummed reflection cycle refused");
            const uint32_t threads[3] = {32, 1, 1};
            std::map<uint32_t, uint32_t> memory;
            if (!strcmp(function, "artifact_probe"))
                memory.emplace(0, 128);
            raw::KernelDispatch dispatch(threads, memory);
            if (!strcmp(function, "semantic_texture") || !strcmp(function, "artifact_probe")) {
                auto *texture = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                   width:32
                                                                                  height:1
                                                                               mipmapped:NO];
                dispatch.texture(0, texture);
            }
            auto *holder = [NBNativeComputeArtifact new];
            holder->_artifact = original;
            objc_setAssociatedObject(descriptor, &computeArtifactKey, holder, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            raw::ComputeRecord compiled, restored;
            std::string why;
            const auto a = direct.kernel(descriptor, dispatch.dispatch(), "artifact seed", compiled, why);
            holder = [NBNativeComputeArtifact new];
            holder->_artifact = imported;
            objc_setAssociatedObject(descriptor, &computeArtifactKey, holder, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            std::fill(bytes.begin(), bytes.end(), 0);
            const auto b = direct.kernel(descriptor, dispatch.dispatch(), "artifact seed", restored, why);
            raw::CacheKey cacheKey;
            check(a == raw::CompileOutcome::Made && b == raw::CompileOutcome::Made &&
                      raw::encodeKernelEntry(cacheKey, &compiled, "") ==
                          raw::encodeKernelEntry(cacheKey, &restored, ""),
                  "imported native dispatch exact after source payload is overwritten");
            objc_setAssociatedObject(descriptor, &computeArtifactKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
        return failures ? 1 : 0;
    }
}
