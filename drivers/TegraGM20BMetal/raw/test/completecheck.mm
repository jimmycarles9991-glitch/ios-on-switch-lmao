// SPDX-License-Identifier: MIT
// completecheck: the runtime's own probe (runtime/RawProbe.h) against the pack. Every Core Animation state of a corpus is made
// through the frontend and m2v over the NVK twin (the mock GPU) and handed to raw::Probe, which draws it in three command buffers
// it captures instead of committing and makes its record in process: every state the pack holds a default record for must come
// out byte for byte the same, every one it set aside refused, a storage-form one refused as such or made as the pack's
// barrier form.
// usage: completecheck <nvk dylib> <cache dir> <default.metallib> <corpus.log> <pack.bin>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <chrono>
#include <cstdio>
#include "../pack/probe_states.h"
#include "../runtime/RawProbe.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#include "record_compare.h"

static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 6) {
            fprintf(stderr,
                    "usage: completecheck <nvk dylib> <cache dir> <default.metallib> <corpus.log> <pack.bin>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        raw::Pack pack;
        std::string err;
        if (!dev || !pack.load(argv[5], 0, err)) {
            fprintf(stderr, "completecheck: %s\n", dev ? err.c_str() : nmb_last_error());
            return 1;
        }
        NSError *error = nil;
        id<MTLLibrary> qc = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        NSString *text = [NSString stringWithContentsOfFile:@(argv[4]) encoding:NSUTF8StringEncoding error:nil];
        if (!qc || !text) {
            fprintf(stderr, "completecheck: library or corpus unread\n");
            return 1;
        }
        NSRegularExpression *lib = [NSRegularExpression regularExpressionWithPattern:@" lib=\\S+" options:0 error:nil];
        raw::Probe probe(dev, pack);
        uint32_t states = 0, same = 0, barriers = 0, storage = 0, refused = 0, bad = 0;
        double ms = 0;
        std::set<std::string> seen;
        for (NSString *l in [text componentsSeparatedByString:@"\n"]) {
            if (![l hasPrefix:@"R "] || ![l containsString:@" ok=1 "] ||
                ![l containsString:@" label=com.apple.coreanimation."])
                continue;
            NSString *plain = [lib stringByReplacingMatchesInString:l
                                                            options:0
                                                              range:NSMakeRange(0, l.length)
                                                       withTemplate:@""];
            probe::State s;
            if (!probe::parseState(qc, plain.UTF8String, s))
                continue;
            // the key as the facade makes it (raw_metal_pipeline_key) and probelog.py reads it: no trailing space
            while (!s.key.empty() && s.key.back() == ' ')
                s.key.pop_back();
            if (!seen.insert(s.key).second)
                continue;
            id<MTLRenderPipelineState> twin = [dev newRenderPipelineStateWithDescriptor:s.d error:&error];
            if (!twin)
                continue;
            states++;
            raw::RuntimeRecord made;
            std::string why;
            const auto began = std::chrono::steady_clock::now();
            const raw::Compiler::Outcome outcome = probe.make(twin, s.d, s.key, made, why);
            ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
            const raw::PackPipeline *pp = pack.find(raw::packKeyHash(s.key));
            const bool barrier = pp && (pp->flags & raw::PipelineBarrierForm);
            std::string verdict;
            if (outcome == raw::Compiler::Outcome::Failed) {
                verdict = " the probe failed (" + why + ")";
            } else if (pp && !barrier) {
                verdict = outcome == raw::Compiler::Outcome::Made ? recordcompare::differences(pack, *pp, made)
                                                                  : " refused (" + why + ")";
                same += verdict.empty();
            } else if (barrier && outcome == raw::Compiler::Outcome::Made) {
                verdict = recordcompare::differences(pack, *pp, made);
                if (!verdict.empty())
                    verdict = " barrier form:" + verdict;
                barriers += verdict.empty();
            } else if (barrier) {
                verdict = outcome == raw::Compiler::Outcome::Refused && why.rfind("storage form", 0) == 0
                              ? ""
                              : " not refused as a storage form (" + why + ")";
                storage += verdict.empty();
            } else {
                verdict = outcome == raw::Compiler::Outcome::Made ? " made, and the pack set it aside" : "";
                if (!verdict.empty())
                    verdict += pack.find(raw::packKeyHash(s.key), raw::PackForm::Barrier) ? " (barrier)" : "";
                refused += verdict.empty();
            }
            if (!verdict.empty()) {
                bad++;
                printf("DIFFERS%s: %s\n", verdict.c_str(), s.key.substr(0, 140).c_str());
            }
        }
        printf(
            "completecheck: %u states probed in process: %u records the same as the pack's, %u the same as its barrier "
            "forms, %u storage forms refused, %u set aside and refused, %u differ; %.1f ms a state\n",
            states, same, barriers, storage, refused, bad, states ? ms / states : 0.0);
        return bad || !states ? 1 : 0;
    }
}
