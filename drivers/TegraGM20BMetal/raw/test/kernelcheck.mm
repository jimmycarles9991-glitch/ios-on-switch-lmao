// SPDX-License-Identifier: MIT
// kernelcheck: the direct compile of every compute dispatch shape of a corpus (runtime/raw_direct_compute.h), its kernel records
// against the pack's. Each kernel the pack holds must come out byte for byte the same (kernel_compare.h); each shape the pack
// set aside is listed with the direct compile's reason (REFUSED lines), or as made (DIFFERS).
// usage: kernelcheck <nvk dylib> <cache dir> <corpus.log> <pack.bin> (PROBE_LIB_ROOT, PROBE_DATA_LIBS as directpack.mm's)
//   KERNELCHECK_SHAPES=1: each kernel's program made at its threadgroup shape, at another of the same threads and at twice
//   (or half) as many, and whether the shape reaches the program (SHAPE lines and a count of each answer)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <map>
#include "../pack/direct_kernels.h"
#include "../runtime/RawDirect.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#include "kernel_compare.h"

namespace {

void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

raw::Compiler::Outcome make(raw::Direct &direct, const probe::Kernel &k, const uint32_t threads[3], std::string &key,
                            raw::ComputeRecord &rec, std::string &why)
{
    probe::Shape shape = k.shape;
    memcpy(shape.threads, threads, sizeof shape.threads);
    key = k.key.substr(0, k.key.find(" | tg=")) + " | tg=" + std::to_string(threads[0]) + "," +
          std::to_string(threads[1]) + "," + std::to_string(threads[2]) + k.key.substr(k.key.find(" | tgmem="));
    std::unique_ptr<raw::KernelDispatch> kd;
    probe::dispatchOf(shape, threads, k.textures, kd);
    return direct.kernel(k.descriptor, kd->dispatch(), key, rec, why);
}

// The same threads in another shape (x and y swapped, or twice as wide and half as tall), and twice as many (half where twice
// passes Metal's 1024)
void otherShapes(const uint32_t t[3], uint32_t same[3], uint32_t twice[3])
{
    memcpy(same, t, 12);
    if (t[1] > 1 && t[0] != t[1]) {
        same[0] = t[1];
        same[1] = t[0];
    } else if (t[1] % 2 == 0) {
        same[0] = 2 * t[0];
        same[1] = t[1] / 2;
    } else if (t[0] % 2 == 0) {
        same[0] = t[0] / 2;
        same[1] = 2 * t[1];
    }
    memcpy(twice, t, 12);
    if (2 * t[0] * t[1] * t[2] <= 1024)
        twice[0] = 2 * t[0];
    else
        twice[t[0] % 2 ? 1 : 0] /= 2;
}

std::string programAnswer(const raw::ComputeRecord &a, const raw::ComputeRecord &b)
{
    if (a.code == b.code && a.data == b.data && !memcmp(a.cbufType, b.cbufType, sizeof a.cbufType))
        return "same";
    return a.code.size() == b.code.size() ? "immediates" : "code size";
}

struct Counts {
    uint32_t kernels = 0, same = 0, refused = 0, bad = 0, met = 0;
    std::map<std::string, uint32_t> reasons, answers;
};

void checkShapes(raw::Direct &direct, const probe::Kernel &k, Counts &n)
{
    std::string key, why;
    raw::ComputeRecord base, same, twice;
    if (make(direct, k, k.shape.threads, key, base, why) != raw::Compiler::Outcome::Made) {
        n.refused++;
        printf("SHAPE %s refused (%s)\n", k.shape.function.c_str(), why.c_str());
        return;
    }
    uint32_t sameThreads[3], twiceThreads[3];
    otherShapes(k.shape.threads, sameThreads, twiceThreads);
    const bool madeSame = make(direct, k, sameThreads, key, same, why) == raw::Compiler::Outcome::Made;
    const bool madeTwice = make(direct, k, twiceThreads, key, twice, why) == raw::Compiler::Outcome::Made;
    const std::string a = madeSame ? programAnswer(base, same) : "refused",
                      b = madeTwice ? programAnswer(base, twice) : "refused";
    n.answers["same threads: " + a]++;
    n.answers["other threads: " + b]++;
    n.kernels++;
    printf("SHAPE %s tg=%u,%u,%u code=%zu: %u,%u,%u %s (%zu), %u,%u,%u %s (%zu)\n", k.shape.function.c_str(),
           k.shape.threads[0], k.shape.threads[1], k.shape.threads[2], base.code.size(), sameThreads[0], sameThreads[1],
           sameThreads[2], a.c_str(), same.code.size(), twiceThreads[0], twiceThreads[1], twiceThreads[2], b.c_str(),
           twice.code.size());
}

void checkPack(raw::Direct &direct, const raw::Pack &pack, const probe::Kernel &k, Counts &n)
{
    raw::ComputeRecord rec;
    std::string why;
    std::unique_ptr<raw::KernelDispatch> kd;
    probe::dispatchOf(k.shape, k.shape.threads, k.textures, kd);
    const raw::Compiler::Outcome outcome = direct.kernel(k.descriptor, kd->dispatch(), k.key, rec, why);
    const raw::PackKernel *pk = pack.findKernel(raw::computeKeyHash(k.key));
    n.kernels++;
    std::string verdict;
    if (outcome == raw::Compiler::Outcome::Failed) {
        verdict = " failed (" + why + ")";
    } else if (pk) {
        n.met++;
        verdict = outcome == raw::Compiler::Outcome::Made ? kernelcompare::differences(pack, *pk, rec)
                                                          : " refused (" + why + ")";
        n.same += verdict.empty();
    } else if (outcome == raw::Compiler::Outcome::Made) {
        verdict = " made, and the pack set it aside";
    } else {
        n.refused++;
        n.reasons[why.substr(0, 90)]++;
        printf("REFUSED (%s): %s %s\n", why.c_str(), k.shape.function.c_str(), k.key.substr(64, 120).c_str());
    }
    if (!verdict.empty()) {
        n.bad++;
        printf("DIFFERS%s: %s %s\n", verdict.c_str(), k.shape.function.c_str(), k.key.substr(64, 120).c_str());
    }
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 5) {
            fprintf(stderr, "usage: kernelcheck <nvk dylib> <cache dir> <corpus.log> <pack.bin>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        raw::Pack pack;
        std::string err;
        if (!dev || !pack.load(argv[4], 0, err)) {
            fprintf(stderr, "kernelcheck: %s\n", dev ? err.c_str() : nmb_last_error());
            return 1;
        }
        NSString *text = [NSString stringWithContentsOfFile:@(argv[3]) encoding:NSUTF8StringEncoding error:nil];
        if (!text) {
            fprintf(stderr, "kernelcheck: corpus unread\n");
            return 1;
        }
        const bool shapes = getenv("KERNELCHECK_SHAPES") != nullptr;
        raw::Direct direct(pack);
        Counts n;
        uint32_t lines = 0;
        probe::eachKernel(
            dev, text,
            [&](const probe::Kernel &k) {
                if (shapes)
                    checkShapes(direct, k, n);
                else
                    checkPack(direct, pack, k, n);
            },
            [&](const std::string &, const std::string &) {
                lines++;
            });
        for (const auto &[why, count] : n.reasons)
            printf("  %u x refused: %s\n", count, why.c_str());
        for (const auto &[answer, count] : n.answers)
            printf("  %u x %s\n", count, answer.c_str());
        const uint64_t packed = pack.bytes(raw::PackKernels) / sizeof(raw::PackKernel);
        if (shapes) {
            printf("kernelcheck shapes: %u kernels made at three shapes, %u refused\n", n.kernels, n.refused);
            return 0;
        }
        printf("kernelcheck: %u kernels made directly (%u lines the probe made none of): %u of the pack's %llu met, %u "
               "the same, %u set aside and refused, %u differ\n",
               n.kernels, lines, n.met, (unsigned long long)packed, n.same, n.refused, n.bad);
        return n.bad || n.met != packed ? 1 : 0;
    }
}
