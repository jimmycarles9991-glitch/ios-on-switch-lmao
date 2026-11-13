// SPDX-License-Identifier: MIT
#import <Foundation/Foundation.h>
#include "native_seed_key.h"
#include "native_seed_manifest.h"
#include "native_seed_compute.h"
#include "native_seed_analysis.h"
#include "../runtime/RawDirect.h"
#include "../runtime/raw_emit.h"
#include "../runtime/raw_cache_io.h"
#include "NativeBridgeMetalFrontend.h"
#include <cerrno>
#include <fstream>
#include <sys/stat.h>

namespace {

void missing(const char *cls, const char *selector)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, selector);
}

using rawseed::hex;

NSString *status(precompile::Outcome outcome)
{
    switch (outcome) {
    case precompile::Made:
        return @"made";
    case precompile::Refused:
        return @"refused";
    case precompile::NoLibrary:
        return @"missing-library";
    case precompile::NoFunction:
        return @"missing-function";
    }
}

bool same(const raw::CacheKey &a, const raw::CacheKey &b)
{
    return a.stack == b.stack && a.key == b.key && !memcmp(a.vertexAir, b.vertexAir, sizeof a.vertexAir) &&
           !memcmp(a.fragmentAir, b.fragmentAir, sizeof a.fragmentAir);
}

struct Compiled {
    raw::CacheKey key;
    NSString *status = nil, *reason = nil, *artifact = nil;
    NSArray *slots = nil;
};

bool compile(raw::Direct &direct, raw::RecordCache &cache, const precompile::RenderInput &input,
             const std::string &output, Compiled &result)
{
    raw::RuntimeRecord record;
    std::string why;
    const auto outcome = direct.make(input.descriptor, result.key.key, record, why, {}, nullptr, true);
    result.status = outcome == raw::CompileOutcome::Made      ? @"made"
                    : outcome == raw::CompileOutcome::Refused ? @"refused"
                                                              : @"failed";
    result.reason = @(why.c_str());
    if (outcome != raw::CompileOutcome::Made)
        return outcome != raw::CompileOutcome::Failed;
    const std::string relative = "raw-" + hex(result.key.stack) + "/" + hex(result.key.name()) + ".rec";
    raw::RuntimeRecord loaded;
    if (!cache.store(result.key, &record, "") ||
        cache.load(result.key, loaded, why) != raw::RecordCache::Found::Record ||
        loaded.keyString != record.keyString || chmod((output + "/" + relative).c_str(), 0644) != 0 ||
        !rawseed::writeSlots(record, result.key.stack, output, result.slots)) {
        result.status = @"failed";
        result.reason = @"native record write/readback failed";
        return false;
    }
    result.artifact = @(relative.c_str());
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc > 1 && !strcmp(argv[1], "--restamp"))
            return rawseed::restamp(argc, argv);
        int first = 1;
        if (argc > 3 && !strcmp(argv[first], "--libs")) {
            precompile::gStore = argv[first + 1];
            first += 2;
        }
        if (argc - first < 6) {
            fprintf(stderr, "usage: native-seed [--libs store] nvk cache QuartzCore.metallib stack output corpus... "
                            "[--derived corpus...]\n");
            return 2;
        }
        char *end = nullptr;
        errno = 0;
        const uint64_t stack = strtoull(argv[first + 3], &end, 16);
        if (errno || !*argv[first + 3] || !end || *end)
            return 2;
        const std::string output = argv[first + 4];
        raw::RecordCache cache;
        if (!rawseed::openOutput(output, stack, cache))
            return 1;
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        precompile::gQuartzCore = argv[first + 2];
        precompile::gDevice = native_bridge_metal_open(argv[first], argv[first + 1], 0, missing);
        if (!precompile::gDevice)
            return 1;
        raw::PackRules rules;
        rules.varying = raw::emittableMethods();
        raw::Direct direct(rules, true);
        NSMutableArray *rows = [NSMutableArray new];
        std::map<uint64_t, Compiled> compiled;
        bool derived = false, failed = false;
        for (int arg = first + 5; arg < argc; ++arg) {
            if (!strcmp(argv[arg], "--derived")) {
                derived = true;
                continue;
            }
            std::ifstream stream(argv[arg]);
            if (!stream) {
                fprintf(stderr, "cannot read %s\n", argv[arg]);
                return 1;
            }
            std::string line;
            size_t lineNumber = 0;
            while (std::getline(stream, line)) {
                ++lineNumber;
                if ((line.rfind("R ", 0) != 0 && line.rfind("D ", 0) != 0 && line.rfind("C ", 0) != 0) ||
                    line.find(" ok=1 ") == std::string::npos)
                    continue;
                @autoreleasepool {
                    if (line[0] == 'D' || line[0] == 'C') {
                        NSArray *dispatchRows = line[0] == 'D' ?
                            rawseed::computeRows(line, stack, direct, cache, output) : @[rawseed::analysisRow(line, output)];
                        for (NSMutableDictionary *row in dispatchRows) {
                            row[@"source"] = @(argv[arg]);
                            row[@"line"] = @(lineNumber);
                            row[@"derived"] = @(derived);
                            row[@"input"] = @(line.c_str());
                            failed |= [row[@"status"] isEqualToString:@"failed"];
                            [rows addObject:row];
                        }
                        continue;
                    }
                    precompile::RenderInput input;
                    NSError *why = nil;
                    const auto outcome = precompile::renderInput(line, input, &why);
                    NSMutableDictionary *row = [@{
                        @"source" : @(argv[arg]),
                        @"kind" : @"render",
                        @"line" : @(lineNumber),
                        @"derived" : @(derived),
                        @"input" : @(line.c_str()),
                        @"status" : status(outcome),
                        @"reason" : why.localizedDescription ?: @"",
                        @"vertexLibrary" : @(input.vertex.library.c_str()),
                        @"fragmentLibrary" : @(input.fragment.library.c_str())
                    } mutableCopy];
                    if (outcome == precompile::Made) {
                        raw::CacheKey key;
                        if (!rawseed::cacheKey(input, stack, key)) {
                            row[@"status"] = @"refused";
                            row[@"reason"] = @"native AIR identity unavailable";
                        } else {
                            const auto found = compiled.find(key.name());
                            if (found != compiled.end() && !same(found->second.key, key)) {
                                row[@"status"] = @"failed";
                                row[@"reason"] = @"native record name collision";
                                failed = true;
                            } else {
                                if (found == compiled.end()) {
                                    Compiled result{key};
                                    failed |= !compile(direct, cache, input, output, result);
                                    compiled.emplace(key.name(), std::move(result));
                                }
                                const auto &result = compiled.at(key.name());
                                rawseed::identify(row, result.key);
                                row[@"status"] = result.status;
                                row[@"reason"] = result.reason;
                                if (result.artifact)
                                    row[@"artifact"] = result.artifact;
                                if (result.slots)
                                    row[@"slots"] = result.slots;
                            }
                        }
                    }
                    [rows addObject:row];
                }
            }
            if (!stream.eof())
                return 1;
        }
        if (!rawseed::writeManifest(output, stack, rows))
            return 1;
        return failed ? 1 : 0;
    }
}
