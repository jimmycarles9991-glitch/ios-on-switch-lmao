// SPDX-License-Identifier: MIT
#import <Foundation/Foundation.h>
#include "native_seed_key.h"
#include "native_seed_manifest.h"
#include "native_seed_compute.h"
#include "../TegraGM20BMetalOffline.h"
#include "../emit.h"
#include "../cache_io.h"
#include <cerrno>
#include <fstream>
#include <sys/stat.h>

namespace {

using gm20b_seed::hex;

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

bool same(const gm20b::CacheKey &a, const gm20b::CacheKey &b)
{
    return a.stack == b.stack && a.key == b.key && !memcmp(a.vertexAir, b.vertexAir, sizeof a.vertexAir) &&
           !memcmp(a.fragmentAir, b.fragmentAir, sizeof a.fragmentAir);
}

struct Compiled {
    gm20b::CacheKey key;
    NSString *status = nil, *reason = nil, *artifact = nil;
    NSArray *slots = nil;
};

bool compile(gm20b::Direct &direct, gm20b::RecordCache &cache, const precompile::RenderInput &input,
             const std::string &output, Compiled &result)
{
    gm20b::RuntimeRecord record;
    std::string why;
    const auto outcome = direct.make(input.descriptor, result.key.key, record, why, {}, nullptr, true);
    result.status = outcome == gm20b::CompileOutcome::Made      ? @"made"
                    : outcome == gm20b::CompileOutcome::Refused ? @"refused"
                                                                : @"failed";
    result.reason = @(why.c_str());
    if (outcome != gm20b::CompileOutcome::Made)
        return outcome != gm20b::CompileOutcome::Failed;
    const std::string relative = "rec-" + hex(result.key.stack) + "/" + hex(result.key.name()) + ".rec";
    gm20b::RuntimeRecord loaded;
    if (!cache.store(result.key, &record, "") ||
        cache.load(result.key, loaded, why) != gm20b::RecordCache::Found::Record ||
        loaded.keyString != record.keyString || !gm20b_seed::withMode(output + "/" + relative, 0644) ||
        !gm20b_seed::writeSlots(record, result.key.stack, output, result.slots)) {
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
            return gm20b_seed::restamp(argc, argv);
        int first = 1;
        if (argc > 3 && !strcmp(argv[first], "--libs")) {
            precompile::gStore = argv[first + 1];
            first += 2;
        }
        if (argc - first < 4) {
            fprintf(stderr, "usage: native-seed [--libs store] QuartzCore.metallib stack output corpus... "
                            "[--derived corpus...]\n");
            return 2;
        }
        char *end = nullptr;
        errno = 0;
        const uint64_t stack = strtoull(argv[first + 1], &end, 16);
        if (errno || !*argv[first + 1] || !end || *end) {
            fprintf(stderr, "native-seed: the stack key %s is not hexadecimal\n", argv[first + 1]);
            return 2;
        }
        const std::string output = argv[first + 2];
        gm20b::RecordCache cache;
        if (!gm20b_seed::openOutput(output, stack, cache))
            return 1;
        precompile::gQuartzCore = argv[first];
        precompile::gDevice = (id<MTLDevice>)[GM20BOfflineDevice new];
        gm20b::PackRules rules;
        rules.varying = gm20b::emittableMethods();
        gm20b::Direct direct(rules, true, offline::translator());
        NSMutableArray *rows = [NSMutableArray new];
        std::map<uint64_t, Compiled> compiled;
        bool derived = false, failed = false;
        for (int arg = first + 3; arg < argc; ++arg) {
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
                if ((line.rfind("R ", 0) != 0 && line.rfind("D ", 0) != 0) || line.find(" ok=1 ") == std::string::npos)
                    continue;
                @autoreleasepool {
                    if (line[0] == 'D') {
                        for (NSMutableDictionary *row in gm20b_seed::computeRows(line, stack, direct, cache, output)) {
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
                        gm20b::CacheKey key;
                        if (!gm20b_seed::cacheKey(input, stack, key)) {
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
                                gm20b_seed::identify(row, result.key);
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
            if (!stream.eof()) {
                gm20b_seed::failed("read", argv[arg], errno);
                return 1;
            }
        }
        if (!gm20b_seed::writeManifest(output, stack, rows))
            return 1;
        if (failed)
            fprintf(stderr, "native-seed: native records failed: their rows in %s/manifest.json\n", output.c_str());
        return failed ? 1 : 0;
    }
}
