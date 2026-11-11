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
// each a u32 length and bytes), its constexpr sampler headers (u32 count, eight words each), which its entries number from 0,
// and its fragment program's split: u32 0, or 1 and its PackSplit (no program indices), its per-pixel program and its prologue
// (as the programs above; the prologue's header 80 bytes of zeros).
// Then per compute dispatch shape (the corpus's D lines, as the kernel probe read them: kernel_shapes.h): "DKRN", the outcome,
// its raw_compute_key.h key and the reason, then for a made kernel its PackKernel (raw_direct_compute.h; program 0), its code,
// shader data and constant buffer types, and its sampler headers, which its sampler entries number from 0 (slot 0x80 | n).
// PROBE_DATA_LIBS names the directory of the corpus's data: libraries.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include "../runtime/raw_emit.h"
#include "NativeBridgeMetalFrontend.h"
#include "TwoPlaneStandIn.h"
#include "bridge/NativeMetalObjects.h"
#include "direct_kernels.h"
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

// The record's head; false: no more follows (it was not made)
bool head(Writer &w, const char *tag, const std::string &key, raw::Compiler::Outcome outcome, const std::string &why)
{
    fwrite(tag, 1, 4, w.f);
    w.u32(outcome == raw::Compiler::Outcome::Made ? 0 : outcome == raw::Compiler::Outcome::Refused ? 1 : 2);
    w.text(key);
    w.text(why);
    return outcome == raw::Compiler::Outcome::Made;
}

void record(Writer &w, const std::string &key, raw::Compiler::Outcome outcome, const std::string &why,
            const raw::RuntimeRecord &rec, const std::map<uint32_t, uint32_t> &registers)
{
    if (!head(w, "DREC", key, outcome, why))
        return;
    w.bytes(&rec.pipeline, sizeof rec.pipeline);
    w.text(rec.keyString);
    std::vector<uint32_t> pairs;
    for (const auto &[m, v] : registers) {
        pairs.push_back(m);
        pairs.push_back(v);
    }
    w.list(pairs);
    auto program = [&](const raw::CapturedProgram &p) {
        w.list(p.image);
        w.list(p.data);
        w.list(p.cbufTypes);
    };
    for (const raw::CapturedProgram &p : rec.programs)
        program(p);
    w.list(rec.samplers);
    w.u32(rec.split);
    if (rec.split) {
        w.bytes(&rec.splitRecord, sizeof rec.splitRecord);
        program(rec.splitPrograms[0]);
        program(rec.splitPrograms[1]);
    }
}

void kernel(Writer &w, const std::string &key, raw::Compiler::Outcome outcome, const std::string &why,
            const raw::ComputeRecord &rec)
{
    if (!head(w, "DKRN", key, outcome, why))
        return;
    w.bytes(&rec.kernel, sizeof rec.kernel);
    w.list(rec.code);
    w.list(rec.data);
    w.bytes(rec.cbufType, rec.cbufs);
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
        uint32_t made = 0, refused = 0, failed = 0, split = 0;
        probe::eachState(dev, qc, own, standIn, probe::corpusLines(text, own != nil),
                         [&](const std::string &line, bool ours, probe::State &s) {
                             raw::RuntimeRecord rec;
                             std::map<uint32_t, uint32_t> registers;
                             std::string why;
                             const raw::Compiler::Outcome outcome =
                                 direct.make(s.d, s.key, rec, why, probe::probeTextures(line, ours), &registers, true);
                             record(w, s.key, outcome, why, rec, registers);
                             split += outcome == raw::Compiler::Outcome::Made && rec.split;
                             (outcome == raw::Compiler::Outcome::Made      ? made
                              : outcome == raw::Compiler::Outcome::Refused ? refused
                                                                           : failed)++;
                         });
        uint32_t kernels[3] = {};
        probe::eachKernel(
            dev, text,
            [&](const probe::Kernel &k) {
                std::unique_ptr<raw::KernelDispatch> ask;
                probe::dispatchOf(k.shape, k.shape.threads, k.textures, ask);
                raw::ComputeRecord rec;
                std::string why;
                const raw::Compiler::Outcome outcome = direct.kernel(k.descriptor, ask->dispatch(), k.key, rec, why);
                kernel(w, k.key, outcome, why, rec);
                kernels[uint32_t(outcome)]++;
            },
            [](const std::string &, const std::string &) {});
        fclose(w.f);
        fprintf(
            stderr,
            "directpack: %u states made (%u split), %u refused, %u failed; %u kernels made, %u refused, %u failed\n",
            made, split, refused, failed, kernels[0], kernels[1], kernels[2]);
        return 0;
    }
}
