// SPDX-License-Identifier: MIT
// vertexcheck: runtime/raw_emit.h's vertex input registers against every probed state of a pack. Each state's vertex function
// is made from its key with this Mac's Metal and the IPSW's library, and its reflection's active vertexAttributes are
// the attribute inputs raw_emit numbers; the registers must be the pack's wherever the hardware reads them.
// usage: vertexcheck <pack.bin> <the IPSW's QuartzCore default.metallib> <bucket dir>...
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include "../pack/probe_states.h"
#include "../runtime/raw_emit.h"
#include "../runtime/raw_pack.h"

namespace {

std::map<uint32_t, uint32_t> registers(const raw::Pack &pack, const raw::PackPipeline &pp)
{
    std::map<uint32_t, uint32_t> regs;
    const uint32_t *blob = pack.section<uint32_t>(raw::PackBlob) + pp.blob;
    for (uint32_t k = 0; k + 1 < pp.blobDwords; k += 2)
        regs[blob[k]] = blob[k + 1];
    return regs;
}

// The state keys a bucket's probe runs drew ("PROBE <i> <key>" lines of probe256.out), trailing space dropped as probelog.py
void keysOf(const std::string &bucket, std::set<std::string> &keys)
{
    for (const char *run : {"/probe256.out", "/barrier/probe256.out"}) {
        std::ifstream in(bucket + run);
        for (std::string line; std::getline(in, line);) {
            if (line.rfind("PROBE ", 0))
                continue;
            const size_t at = line.find(' ', 6);
            std::string key = at == std::string::npos ? "" : line.substr(at + 1);
            while (!key.empty() && key.back() == ' ')
                key.pop_back();
            if (!key.empty())
                keys.insert(key);
        }
    }
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4) {
            fprintf(stderr, "usage: vertexcheck <pack.bin> <default.metallib> <bucket dir>...\n");
            return 2;
        }
        raw::Pack pack;
        std::string err;
        if (!pack.load(argv[1], 0, err)) {
            fprintf(stderr, "vertexcheck: %s\n", err.c_str());
            return 1;
        }
        NSError *error = nil;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[2])] error:&error];
        if (!library) {
            fprintf(stderr, "vertexcheck: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }
        std::set<std::string> keys;
        for (int b = 3; b < argc; b++)
            keysOf(argv[b], keys);
        uint64_t states = 0, unmade = 0, refused = 0, compared = 0, differ = 0, unreadDiffer = 0;
        std::map<std::string, uint64_t> reasons;
        for (const std::string &key : keys) {
            const raw::PackPipeline *pp = pack.find(raw::Pack::fnv(key.data(), key.size()));
            if (!pp)
                continue;
            probe::State s;
            if (!probe::parseState(library, "R ok=1 " + key + " | ", s)) {
                unmade++;
                continue;
            }
            states++;
            std::vector<uint32_t> attributes;
            for (MTLVertexAttribute *a in s.d.vertexFunction.vertexAttributes)
                if (a.active)
                    attributes.push_back(uint32_t(a.attributeIndex));
            std::vector<raw::EmitRegister> made;
            std::string why;
            if (!raw::emitVertexInput(pack.string(pp->keyString), attributes, made, why)) {
                refused++;
                reasons[why.substr(0, 60)]++;
                continue;
            }
            const std::map<uint32_t, uint32_t> regs = registers(pack, *pp);
            for (const raw::EmitRegister &r : made) {
                const auto it = regs.find(r.method);
                if (it == regs.end())
                    continue;
                compared++;
                if (it->second == r.value)
                    continue;
                if (!r.read) {
                    unreadDiffer++;
                    continue;
                }
                if (!differ++ || differ < 4)
                    printf("DIFFERS %04x pack %x made %x: %s\n", r.method, it->second, r.value,
                           key.substr(0, 200).c_str());
            }
        }
        printf("vertexcheck %s: %zu keys probed, %llu states of the pack made with this Mac's Metal (%llu not), %llu "
               "refused, %llu values compared: %llu read ones differ, %llu unread ones differ\n",
               argv[1], keys.size(), (unsigned long long)states, (unsigned long long)unmade,
               (unsigned long long)refused, (unsigned long long)compared, (unsigned long long)differ,
               (unsigned long long)unreadDiffer);
        for (const auto &[why, n] : reasons)
            printf("  refused x%llu: %s\n", (unsigned long long)n, why.c_str());
        return differ ? 1 : 0;
    }
}
