// SPDX-License-Identifier: MIT
// The pack generator's records made directly (runtime/RawDirect.h): every state of a bucket's corpus compiled from m2v's
// translation and NVK's stage compile, as the runtime makes a state the pack lacks, with no pipeline, draw or capture.
// directlink.py makes the pack of them.
// usage: directpack <nvk dylib> <cache dir> <default.metallib> <corpus.log> <records.bin>
// (PROBE_OWN_LIB, PROBE_LIB_ROOT and PROBE_LABEL as probe.mm reads them)
//
// records.bin, little-endian, per state: "DREC", the outcome (0 made, 1 refused, 2 failed), the key and the reason (u32 length,
// bytes), then for a made state its PackPipeline (raw_pack.h; vs 0, fs 1, no blob), its key string, its registers (u32 count,
// method and value pairs: raw_emit's), its vertex and fragment programs (header + code, shader data, constant buffer types:
// each a u32 length and bytes) and its constexpr sampler headers (u32 count, eight words each), which its entries number from 0.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include "../runtime/raw_emit.h"
#include "NativeBridgeMetalFrontend.h"
#include "TwoPlaneStandIn.h"
#include "bridge/NativeMetalObjects.h"
#include "direct_states.h"

namespace {

void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

struct Writer {
    FILE *f;

    void u32(uint32_t v)
    {
        fwrite(&v, 4, 1, f);
    }

    void bytes(const void *p, size_t n)
    {
        u32(uint32_t(n));
        fwrite(p, 1, n, f);
    }

    void text(const std::string &s)
    {
        bytes(s.data(), s.size());
    }

    template <typename T> void list(const std::vector<T> &v)
    {
        bytes(v.data(), v.size() * sizeof(T));
    }
};

void record(Writer &w, const std::string &key, raw::Compiler::Outcome outcome, const std::string &why,
            const raw::RuntimeRecord &rec, const std::map<uint32_t, uint32_t> &registers)
{
    fwrite("DREC", 1, 4, w.f);
    w.u32(outcome == raw::Compiler::Outcome::Made ? 0 : outcome == raw::Compiler::Outcome::Refused ? 1 : 2);
    w.text(key);
    w.text(why);
    if (outcome != raw::Compiler::Outcome::Made)
        return;
    w.bytes(&rec.pipeline, sizeof rec.pipeline);
    w.text(rec.keyString);
    std::vector<uint32_t> pairs;
    for (const auto &[m, v] : registers) {
        pairs.push_back(m);
        pairs.push_back(v);
    }
    w.list(pairs);
    for (const raw::CapturedProgram &p : rec.programs) {
        w.list(p.image);
        w.list(p.data);
        w.list(p.cbufTypes);
    }
    w.list(rec.samplers);
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 6) {
            fprintf(stderr,
                    "usage: directpack <nvk dylib> <cache dir> <default.metallib> <corpus.log> <records.bin>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        NSError *error = nil;
        id<MTLLibrary> qc = dev ? [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error] : nil;
        NSString *text = [NSString stringWithContentsOfFile:@(argv[4]) encoding:NSUTF8StringEncoding error:nil];
        Writer w = {fopen(argv[5], "wb")};
        if (!qc || !text || !w.f) {
            fprintf(stderr, "directpack: %s\n", dev ? "library, corpus or output unread" : nmb_last_error());
            return 1;
        }
        id<MTLLibrary> own = nil, standIn = nil;
        if (const char *path = getenv("PROBE_OWN_LIB"); path && *path) {
            own = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&error];
            dispatch_data_t data = dispatch_data_create(kTwoPlaneStandInLibrary, sizeof kTwoPlaneStandInLibrary,
                                                        nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            standIn = own ? [dev newLibraryWithData:data error:&error] : nil;
            if (!own || !standIn) {
                fprintf(stderr, "directpack: own library: %s\n", error.localizedDescription.UTF8String);
                return 1;
            }
        }
        // every register raw_emit writes, over no baseline: directlink.py sets them over the context
        raw::PackRules rules;
        rules.varying = raw::emittableMethods();
        raw::Direct direct(std::move(rules));
        uint32_t made = 0, refused = 0, failed = 0;
        probe::eachState(dev, qc, own, standIn, probe::corpusLines(text, own != nil),
                         [&](const std::string &line, bool ours, probe::State &s) {
                             raw::RuntimeRecord rec;
                             std::map<uint32_t, uint32_t> registers;
                             std::string why;
                             const raw::Compiler::Outcome outcome =
                                 direct.make(s.d, s.key, rec, why, probe::probeTextures(line, ours), &registers);
                             record(w, s.key, outcome, why, rec, registers);
                             (outcome == raw::Compiler::Outcome::Made      ? made
                              : outcome == raw::Compiler::Outcome::Refused ? refused
                                                                           : failed)++;
                         });
        fclose(w.f);
        fprintf(stderr, "directpack: %u states made, %u refused, %u failed\n", made, refused, failed);
        return 0;
    }
}
